#!/usr/bin/env bash
set -euo pipefail

repo=/opt/infiniswap
module=$repo/infiniswap_bd/infiniswap.ko
daemon=$repo/build/daemon/infiniswap-daemon
artifacts=/var/tmp/infiniswap-vm-artifacts
configfs=/sys/kernel/config
root=$configfs/infiniswap
name=infiniswap-vm
 group=$root/$name
device=/dev/$name
backing=/dev/vdb
chunk_bytes=$((1024 * 1024 * 1024))
fault_comment=infiniswap-vm-network-fault

mkdir -p "$artifacts"

fail() {
  echo "guest validation failed: $*" >&2
  exit 1
}

wait_for_path() {
  local path=$1 expected=$2 attempt
  for ((attempt = 0; attempt < 300; attempt++)); do
    if [[ $expected == present && -e $path ]]; then
      return 0
    fi
    if [[ $expected == absent && ! -e $path ]]; then
      return 0
    fi
    sleep 0.1
  done
  fail "$path did not become $expected"
}

wait_for_field() {
  local field=$1 expected=$2 attempts=${3:-300} attempt actual=missing
  for ((attempt = 0; attempt < attempts; attempt++)); do
    actual=$(cat "$group/$field" 2>/dev/null || echo missing)
    if [[ $actual == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  fail "$field did not become $expected (got $actual)"
}

wait_for_counter() {
  local field=$1 minimum=$2 attempts=${3:-400} attempt actual=0
  for ((attempt = 0; attempt < attempts; attempt++)); do
    actual=$(cat "$group/$field" 2>/dev/null || echo 0)
    ((actual >= minimum)) && return 0
    sleep 0.05
  done
  fail "$field did not reach $minimum (got $actual)"
}

wait_for_exclusion() {
  local provider_id=$1 attempt actual=
  for ((attempt = 0; attempt < 400; attempt++)); do
    actual=$(cat "$group/provider_exclusions" 2>/dev/null || true)
    [[ $actual == *"$provider_id"* ]] && return 0
    sleep 0.05
  done
  fail "$provider_id was not excluded (got '$actual')"
}

mount_configfs() {
  if ! mountpoint -q "$configfs"; then
    mount -t configfs none "$configfs"
  fi
}

load_module() {
  mount_configfs
  if [[ ! -d /sys/module/infiniswap ]]; then
    insmod "$module"
  fi
  [[ -d $root ]] || fail "Infiniswap configfs subsystem is absent"
}

stop_device() {
  local attempt
  [[ -d $group ]] || return 0
  if grep -q "^$device " /proc/swaps; then
    swapoff "$device"
  fi
  for ((attempt = 0; attempt < 300; attempt++)); do
    if printf 'stop\n' >"$group/state" 2>/dev/null; then
      break
    fi
    sleep 0.1
  done
  [[ $(cat "$group/state" 2>/dev/null || true) == stopped ]] ||
    fail "device did not stop"
  wait_for_path "$device" absent
  rmdir "$group"
}

psk_corrupt() {
  local path=$1
  cp -- "$path" "$path.correct"
  printf '%064d\n' 0 >"$path"
  chmod 0600 "$path"
}

psk_restore() {
  local path=$1
  [[ -f $path.correct ]] || fail "PSK backup is absent"
  mv -- "$path.correct" "$path"
  chmod 0600 "$path"
}

provider_start() {
  local port=$1 psk_file=$2 provider_id=${3:-provider-vm} psk
  psk=$(<"$psk_file")
  [[ $psk =~ ^[[:xdigit:]]{64}$ ]] || fail "invalid Provider PSK file"
  cat > /etc/infiniswap-vm-memory.conf <<'EOF'
version = 1
host_reserve_gib = 0
max_opportunistic_gib = 2
max_committed_gib = 2
EOF
  cat > /etc/infiniswap-vm-consumers.conf <<EOF
version = 1

[consumer:consumer-vm]
current_key_id = key-vm
current_psk_hex = $psk
max_connections = 8
max_opportunistic_gib = 2
max_committed_gib = 2
revoked = false
EOF
  chmod 0600 /etc/infiniswap-vm-consumers.conf
  systemctl stop infiniswap-vm-provider.service 2>/dev/null || true
  systemctl reset-failed infiniswap-vm-provider.service 2>/dev/null || true
  systemd-run --quiet --unit=infiniswap-vm-provider \
    --property=LimitMEMLOCK=infinity --property=Nice=5 \
    --setenv="INFINISWAP_PROVIDER_ID=$provider_id" -- \
    "$daemon" :: "$port" /etc/infiniswap-vm-memory.conf \
    /etc/infiniswap-vm-consumers.conf
  for _ in {1..100}; do
    systemctl is-active --quiet infiniswap-vm-provider.service && return 0
    sleep 0.1
  done
  journalctl -u infiniswap-vm-provider.service --no-pager -n 100 >&2 || true
  fail "Memory Provider did not start"
}

setup_rxe() {
  local rail=$1 mac=${2,,} address=$3 netdev=
  for candidate in /sys/class/net/*; do
    [[ -r $candidate/address ]] || continue
    if [[ $(<"$candidate/address") == "$mac" ]]; then
      netdev=${candidate##*/}
      break
    fi
  done
  [[ -n $netdev ]] || fail "could not find netdev with MAC $mac"
  ip link set "$netdev" up
  ip address flush dev "$netdev"
  ip address add "$address" dev "$netdev"
  modprobe rdma_rxe
  rdma link delete "$rail" 2>/dev/null || true
  rdma link add "$rail" type rxe netdev "$netdev"
  rdma link show "$rail/1"
}

create_device() {
  local mode=$1 policy=$2 group_backing=$3 capacity=$4
  shift 4
  local -a specs=("$@") providers=()
  local spec provider_id address port rail psk_file psk numa provider_list=

  stop_device
  load_module
  mkdir "$group"
  printf '%s\n' "$mode" >"$group/mode"
  if [[ $mode == backed ]]; then
    printf '%s\n' "$policy" >"$group/acknowledgement_policy"
    printf '%s\n' "$group_backing" >"$group/backing_store"
    printf '1\n' >"$group/hot_range_threshold"
    printf '1\n' >"$group/hot_range_read_weight"
    printf '4\n' >"$group/hot_range_write_weight"
  else
    printf '1\n' >"$group/remote_only_eligible"
  fi
  printf '%s\n' "$capacity" >"$group/capacity_bytes"
  printf '2000\n' >"$group/provider_failure_deadline_ms"
  printf 'consumer-vm\n' >"$group/consumer_id"
  printf '100\n' >"$group/swap_priority"

  for spec in "${specs[@]}"; do
    IFS='|' read -r provider_id address port rail psk_file <<<"$spec"
    providers+=("$provider_id")
  done
  provider_list=$(IFS=,; echo "${providers[*]}")
  printf '%s\n' "$provider_list" >"$group/providers"
  printf '%s\n' "${#providers[@]}" >"$group/placement_sample_size"
  printf '7\n' >"$group/placement_seed"

  for spec in "${specs[@]}"; do
    IFS='|' read -r provider_id address port rail psk_file <<<"$spec"
    psk=$(<"$psk_file")
    [[ $psk =~ ^[[:xdigit:]]{64}$ ]] || fail "invalid Consumer PSK file"
    numa=$(cat "/sys/class/infiniband/$rail/device/numa_node" 2>/dev/null || echo -1)
    printf '%s\n' "$provider_id" >"$group/provider_bind"
    printf '%s\n' "$address" >"$group/provider_address"
    printf '%s\n' "$port" >"$group/provider_port"
    printf '%s\n' "$rail" >"$group/rdma_device"
    printf '1\n' >"$group/rdma_port"
    printf '%s\n' "$numa" >"$group/rdma_numa_node"
    printf 'key-vm\n' >"$group/provider_key_id"
    printf '%s\n' "$psk" >"$group/provider_psk"
    printf '100\n' >"$group/placement_weight"
  done
  printf 'activate\n' >"$group/state"
  wait_for_path "$device" present
  wait_for_field connection_state connected
}

write_pattern() {
  local label=$1 block=${2:-0} flush=${3:-yes} count=${4:-64}
  dd if=/dev/urandom of="$artifacts/$label.pattern" bs=4096 count="$count" status=none
  sync "$artifacts/$label.pattern"
  if [[ $flush == yes ]]; then
    timeout 20 dd if="$artifacts/$label.pattern" of="$device" bs=4096 count="$count" \
      seek="$block" oflag=direct conv=fsync status=none
  else
    timeout 20 dd if="$artifacts/$label.pattern" of="$device" bs=4096 count="$count" \
      seek="$block" oflag=direct status=none
  fi
}

read_pattern() {
  local label=$1 block=${2:-0} count=${3:-64}
  timeout 20 dd if="$device" of="$artifacts/$label.actual" bs=4096 count="$count" \
    skip="$block" iflag=direct status=none
  cmp "$artifacts/$label.pattern" "$artifacts/$label.actual" ||
    fail "$label data mismatch"
}

heat_ranges() {
  local count=$1 index mapped placements
  for ((index = 0; index < count; index++)); do
    write_pattern "heat-$index" "$((index * chunk_bytes / 4096))"
    read_pattern "heat-$index" "$((index * chunk_bytes / 4096))"
  done
  for _ in {1..300}; do
    mapped=$(cat "$group/mapped_remote_chunks")
    ((mapped >= count)) && break
    sleep 0.1
  done
  ((mapped >= count)) || fail "expected $count mapped Remote Chunks, got $mapped"
  placements=$(cat "$group/remote_chunk_placements")
  printf '%s\n' "$placements" | tee "$artifacts/placements.txt"
  if ((count > 1)); then
    first=${placements%% *}
    first=${first#*:}
    second=${placements##* }
    second=${second#*:}
    [[ $first != "$second" ]] || fail "Power-of-d did not spread chunks: $placements"
  fi
}

verify_remote_only_heartbeats() {
  local intervals=${1:-5} deadline_ms heartbeat_ms wait_ms
  deadline_ms=$(<"$group/provider_failure_deadline_ms")
  heartbeat_ms=$((deadline_ms / 3))
  ((heartbeat_ms > 0)) || heartbeat_ms=1
  wait_ms=$((heartbeat_ms * intervals))
  python3 - "$wait_ms" <<'PY'
import sys
import time

time.sleep(int(sys.argv[1]) / 1000)
PY
  [[ $(<"$group/connection_state") == connected ]] ||
    fail "Remote-Only disconnected while heartbeats were expected"
  [[ $(<"$group/operational_state") == healthy ]] ||
    fail "Remote-Only left its healthy operational state"
  [[ $(<"$group/provider_timeouts_total") == 0 ]] ||
    fail "Remote-Only heartbeat validation observed a Provider timeout"
}

run_fio() {
  local label=$1 duration=${2:-0} rw=randrw
  local -a args
  [[ $label == *remote-only* ]] && rw=write
  args=(--name="$label" --filename="$device" --direct=1 --ioengine=libaio
    --rw="$rw" --bs=4k --iodepth=32 --size=64m
    --verify=crc32c --do_verify=1 --verify_fatal=1 --group_reporting
    --output-format=json --output="$artifacts/fio-$label.json")
  if [[ $rw == randrw ]]; then
    args+=(--rwmixread=60)
  fi
  if ((duration > 0)); then
    args+=(--time_based=1 --runtime="$duration")
  fi
  timeout "$((duration + 300))" fio "${args[@]}"
}

assert_alert() {
  local label=$1 alert_name=$2 expected=$3
  local output="$artifacts/alerts-$label.json"
  "$repo/bin/infiniswapctl" alerts "$name" --json >"$output"
  python3 - "$output" "$alert_name" "$expected" <<'PY'
import json
import sys

path, alert_name, expected = sys.argv[1:]
with open(path, encoding="ascii") as source:
    names = {alert["name"] for alert in json.load(source)["alerts"]}
present = alert_name in names
if present != (expected == "present"):
    raise SystemExit(
        "alert %s was %s; expected %s" %
        (alert_name, "present" if present else "absent", expected)
    )
PY
}

snapshot() {
  local label=$1
  if [[ -d $group ]]; then
    "$repo/bin/infiniswapctl" status "$name" --json >"$artifacts/status-$label.json"
    "$repo/bin/infiniswapctl" metrics "$name" >"$artifacts/metrics-$label.prom"
    "$repo/bin/infiniswapctl" alerts "$name" --json >"$artifacts/alerts-$label.json"
    (
      cd "$group"
      for attribute in state operational_state connection_state backing_state \
        provider_exclusions provider_runtime_status remote_chunk_placements \
        mapped_remote_chunks remote_capacity_bytes provider_timeouts_total \
        remote_lost_transitions_total backing_degraded_transitions_total \
        rejected_writes_total authentication_failures_total \
        admission_rejections_total io_requests_total io_completed_total \
        io_errors_total inflight_io oldest_inflight_ms last_error; do
        [[ -r $attribute ]] && printf '%s=%s\n' "$attribute" "$(<$attribute)"
      done
    ) >"$artifacts/configfs-$label.txt"
  fi
}

swap_pressure() {
  local before after
  mkswap -f "$device" >"$artifacts/mkswap.txt"
  swapon --priority 100 "$device"
  before=$(awk '/^pswpout / {print $2}' /proc/vmstat)
  systemd-run --quiet --wait --pipe --collect \
    --unit="infiniswap-vm-pressure-$$" \
    --property=MemoryMax=512M --property=MemorySwapMax=512M \
    python3 - "$artifacts/swap-pressure.json" <<'PY'
import json
import os
import sys
import time

result_path = sys.argv[1]
chunk_size = 32 * 1024 * 1024


def device_swap_used_bytes():
    with open("/proc/swaps", encoding="ascii") as source:
        for line in source.readlines()[1:]:
            fields = line.split()
            if fields and fields[0] == "/dev/infiniswap-vm":
                return int(fields[3]) * 1024
    raise RuntimeError("Infiniswap Device is not active swap")


with open("/proc/meminfo", encoding="ascii") as source:
    values = {line.split(":", 1)[0]: int(line.split()[1]) for line in source}
swap_free_start = values["SwapFree"] * 1024
device_used_start = device_swap_used_bytes()
bytes_target = 768 * 1024 * 1024
chunks = []
status = "no-swap-observed"
try:
    with open("/proc/self/oom_score_adj", "w", encoding="ascii") as target:
        target.write("1000\n")
    for _ in range(bytes_target // chunk_size):
        chunk = bytearray(chunk_size)
        for offset in range(0, len(chunk), 4096):
            chunk[offset] = 0x5A
        chunks.append(chunk)
        if device_swap_used_bytes() >= device_used_start + 32 * 1024 * 1024:
            status = "swap-observed"
            break
    time.sleep(2)
finally:
    payload = {
        "allocated_bytes": len(chunks) * chunk_size,
        "device_used_start_bytes": device_used_start,
        "status": status,
        "swap_free_start_bytes": swap_free_start,
    }
    with open(result_path, "w", encoding="utf-8") as target:
        json.dump(payload, target, sort_keys=True)
        target.write("\n")
if status != "swap-observed":
    raise SystemExit("memory pressure did not use the Infiniswap Device")
PY
  after=$(awk '/^pswpout / {print $2}' /proc/vmstat)
  swapoff "$device"
  ((after > before)) || fail "swap pressure did not increment pswpout"
}

backing_create() {
  dmsetup remove infiniswap-vm-backing 2>/dev/null || true
  printf '0 %s delay %s 0 0 %s 0 200\n' \
    "$((2 * chunk_bytes / 512))" "$backing" "$backing" |
    dmsetup create infiniswap-vm-backing
}

backing_fail() {
  dmsetup suspend infiniswap-vm-backing
  printf '0 %s error\n' "$((2 * chunk_bytes / 512))" |
    dmsetup reload infiniswap-vm-backing
  dmsetup resume infiniswap-vm-backing
}

backing_restore() {
  [[ -e /dev/mapper/infiniswap-vm-backing ]] || return 0
  dmsetup suspend infiniswap-vm-backing
  printf '0 %s linear %s 0\n' "$((2 * chunk_bytes / 512))" "$backing" |
    dmsetup reload infiniswap-vm-backing
  dmsetup resume infiniswap-vm-backing
}

network_fault() {
  local action=$1 address=$2
  case $action in
    add)
      iptables -w -I INPUT -p udp -s "$address" --dport 4791 \
        -m comment --comment "$fault_comment" -j DROP
      ;;
    clear)
      while iptables -w -D INPUT -p udp -s "$address" --dport 4791 \
        -m comment --comment "$fault_comment" -j DROP 2>/dev/null; do :; done
      ;;
    *) fail "unknown network fault action: $action" ;;
  esac
}

check_kernel() {
  local start=1
  [[ -r $artifacts/dmesg-start ]] && start=$(( $(<$artifacts/dmesg-start) + 1 ))
  dmesg | tail -n "+$start" >"$artifacts/kernel-current.log"
  if grep -Eiq 'BUG:|WARNING:|Oops:|kernel panic|hung task|blocked for more than|use-after-free|general protection fault|refcount.*underflow' \
      "$artifacts/kernel-current.log"; then
    fail "kernel diagnostics contain a warning, oops, panic, or hung task"
  fi
}

mark_kernel() {
  dmesg | wc -l >"$artifacts/dmesg-start"
}

safe_module_reload() {
  [[ ! -d $group ]] || fail "cannot reload module with a configured device"
  if [[ -d /sys/module/infiniswap ]]; then
    rmmod infiniswap
  fi
  insmod "$module"
  [[ -d $root ]] || fail "configfs subsystem did not return after module reload"
}

soak() {
  local label=$1 seconds=$2 started now iteration=0 slice
  started=$(date +%s)
  while true; do
    now=$(date +%s)
    ((iteration > 0 && now - started >= seconds)) && break
    slice=0
    if ((seconds > 0)); then
      slice=$((seconds - (now - started)))
      ((slice > 300)) && slice=300
      ((slice < 1)) && slice=1
    fi
    run_fio "soak-$label-$iteration" "$slice"
    snapshot "soak-$label-$iteration"
    check_kernel
    iteration=$((iteration + 1))
  done
  printf '{"duration_seconds":%s,"iterations":%s}\n' \
    "$(($(date +%s) - started))" "$iteration" >"$artifacts/soak-$label.json"
}

collect() {
  if [[ -d $group ]]; then
    snapshot collected 2>"$artifacts/status-collected.error.txt" || true
  fi
  dmesg >"$artifacts/dmesg.log" || true
  journalctl --no-pager >"$artifacts/journal.log" || true
  ip -details address >"$artifacts/ip-address.txt" || true
  ip route >"$artifacts/ip-route.txt" || true
  rdma link show >"$artifacts/rdma-links.txt" || true
  lsblk -O --json >"$artifacts/lsblk.json" || true
  cp /proc/swaps "$artifacts/swaps.txt" || true
  lsmod >"$artifacts/modules.txt" || true
  systemctl status infiniswap-vm-provider.service --no-pager \
    >"$artifacts/provider-service.txt" 2>&1 || true
  if systemctl is-active --quiet infiniswap-vm-provider.service; then
    "$repo/bin/infiniswapctl" provider-status --json \
      >"$artifacts/provider-status.json" 2>"$artifacts/provider-status.error.txt" || true
    "$repo/bin/infiniswapctl" provider-metrics \
      >"$artifacts/provider-metrics.prom" 2>"$artifacts/provider-metrics.error.txt" || true
  fi
}

cleanup_guest() {
  set +e
  for address in "$@"; do
    network_fault clear "$address"
  done
  if grep -q "^$device " /proc/swaps; then
    swapoff "$device"
  fi
  backing_restore
  stop_device
  dmsetup remove infiniswap-vm-backing 2>/dev/null || true
  if [[ -d /sys/module/infiniswap ]]; then
    rmmod infiniswap
  fi
  systemctl stop infiniswap-vm-provider.service 2>/dev/null || true
  collect
}

command=${1:-}
shift || true
case $command in
  setup-rxe) setup_rxe "$@" ;;
  provider-start) provider_start "$@" ;;
  psk-corrupt) psk_corrupt "$@" ;;
  psk-restore) psk_restore "$@" ;;
  provider-stop) systemctl stop infiniswap-vm-provider.service ;;
  provider-kill)
    pid=$(systemctl show -p MainPID --value infiniswap-vm-provider.service)
    [[ $pid =~ ^[1-9][0-9]*$ ]] || fail "Provider has no live PID"
    kill -KILL "$pid"
    for _ in {1..100}; do
      kill -0 "$pid" 2>/dev/null || exit 0
      sleep 0.05
    done
    fail "Provider process did not die"
    ;;
  module-load) load_module ;;
  device-create) create_device "$@" ;;
  device-stop) stop_device ;;
  write-pattern) write_pattern "$@" ;;
  read-pattern) read_pattern "$@" ;;
  heat-ranges) heat_ranges "$@" ;;
  fio) run_fio "$@" ;;
  verify-remote-only-heartbeats) verify_remote_only_heartbeats "$@" ;;
  snapshot) snapshot "$@" ;;
  assert-alert) assert_alert "$@" ;;
  swap-pressure) swap_pressure ;;
  backing-create) backing_create ;;
  backing-fail) backing_fail ;;
  backing-restore) backing_restore ;;
  backing-remove) dmsetup remove infiniswap-vm-backing ;;
  network-fault) network_fault "$@" ;;
  wait-field) wait_for_field "$@" ;;
  wait-counter) wait_for_counter "$@" ;;
  wait-exclusion) wait_for_exclusion "$@" ;;
  expect-io-failure)
    if timeout 10 dd if=/dev/zero of="$device" bs=4096 count=1 oflag=direct status=none; then
      fail "terminal device accepted I/O"
    fi
    ;;
  mark-kernel) mark_kernel ;;
  check-kernel) check_kernel ;;
  safe-module-reload) safe_module_reload ;;
  soak) soak "$@" ;;
  collect) collect ;;
  cleanup) cleanup_guest "$@" ;;
  kernel)
    uname -r
    ;;
  build-consumer)
    make -C "$repo/infiniswap_bd" "KDIR=/lib/modules/$(uname -r)/build" -j2 modules
    ;;
  build-provider)
    cmake -S "$repo/infiniswap_daemon" -B "$repo/build/daemon" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "$repo/build/daemon" --parallel 2
    ;;
  *) fail "unknown command: $command" ;;
esac
