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
  local host_reserve_gib=${4:-0} max_opportunistic_gib=${5:-2}
  local max_committed_gib=${6:-2} available_gib

  systemctl stop infiniswap-vm-provider.service 2>/dev/null || true
  systemctl reset-failed infiniswap-vm-provider.service 2>/dev/null || true
  if [[ $host_reserve_gib == auto ]]; then
    available_gib=$(awk '/^MemAvailable:/ {print int($2 / 1048576)}' /proc/meminfo)
    [[ $available_gib =~ ^[0-9]+$ ]] || fail "could not read Provider MemAvailable"
    ((available_gib >= 2)) || fail "Provider needs at least 2 GiB available"
    host_reserve_gib=$((available_gib - 1))
  fi
  [[ $host_reserve_gib =~ ^[0-9]+$ &&
     $max_opportunistic_gib =~ ^[0-9]+$ &&
     $max_committed_gib =~ ^[0-9]+$ ]] ||
    fail "invalid Provider memory policy"
  ((max_opportunistic_gib + max_committed_gib > 0)) ||
    fail "Provider memory policy has no Remote Chunk capacity"
  psk=$(<"$psk_file")
  [[ $psk =~ ^[[:xdigit:]]{64}$ ]] || fail "invalid Provider PSK file"
  cat > /etc/infiniswap-vm-memory.conf <<EOF
version = 1
host_reserve_gib = $host_reserve_gib
max_opportunistic_gib = $max_opportunistic_gib
max_committed_gib = $max_committed_gib
EOF
  cat > /etc/infiniswap-vm-consumers.conf <<EOF
version = 1

[consumer:consumer-vm]
current_key_id = key-vm
current_psk_hex = $psk
max_connections = 8
max_opportunistic_gib = $max_opportunistic_gib
max_committed_gib = $max_committed_gib
revoked = false
EOF
  chmod 0600 /etc/infiniswap-vm-consumers.conf
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
  local mode=$1 policy=$2 group_backing=$3 capacity=$4 failure_deadline_ms=$5
  shift 5
  local wait_for_connection=yes
  if [[ ${1:-} == --allow-not-connected ]]; then
    wait_for_connection=no
    shift
  fi
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
  [[ $failure_deadline_ms =~ ^[0-9]+$ ]] ||
    fail "invalid Provider Failure Deadline"
  printf '%s\n' "$capacity" >"$group/capacity_bytes"
  printf '%s\n' "$failure_deadline_ms" >"$group/provider_failure_deadline_ms"
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
  if [[ $wait_for_connection == yes ]]; then
    wait_for_field connection_state connected
  fi
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

remote_chunk_contracts() {
  local binary=$artifacts/remote-chunk-certification-test
  local report=$artifacts/remote-chunk-contracts.json

  cc -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread \
    "$repo/infiniswap_bd/is_remote_chunk.c" \
    "$repo/infiniswap_bd/tests/remote-chunk-test.c" -o "$binary"
  "$binary" --certification-report >"$report"
  python3 - "$report" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="ascii") as source:
    report = json.load(source)
expected = {
    "atomic_batches",
    "eviction_drain",
    "generation_safety",
    "remote_only",
}
if (
    report.get("kind") != "infiniswap.remote-chunk-certification"
    or report.get("status") != "passed"
    or set(report.get("checks", {})) != expected
    or any(value != "passed" for value in report["checks"].values())
):
    raise SystemExit("Remote Chunk certification contracts failed")
PY
}

rdma_delay() {
  local action=$1 rail=$2 delay_ms=${3:-0} netdev
  local mark=28 comment=infiniswap-vm-rdma-data-delay

  command -v tc >/dev/null || fail "tc is required for RDMA delay injection"
  command -v iptables >/dev/null || fail "iptables is required for RDMA delay injection"
  netdev=$(rdma link show "$rail/1" |
    awk '{for (i=1; i<=NF; i++) if ($i == "netdev") print $(i+1)}')
  [[ -n $netdev ]] || fail "could not resolve the netdev for $rail/1"
  case $action in
    add)
      [[ $delay_ms =~ ^[1-9][0-9]*$ ]] || fail "invalid RDMA delay"
      while iptables -w -t mangle -D OUTPUT -o "$netdev" -p udp \
        --dport 4791 -m length --length 128:65535 -m comment \
        --comment "$comment" -j MARK --set-mark "$mark" 2>/dev/null; do :; done
      tc qdisc del dev "$netdev" root 2>/dev/null || true
      tc qdisc add dev "$netdev" root handle 1: prio bands 2 \
        priomap 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
      tc qdisc add dev "$netdev" parent 1:2 handle 20: netem \
        delay "${delay_ms}ms"
      tc filter add dev "$netdev" protocol ip parent 1: prio 1 \
        handle "$mark" fw flowid 1:2
      iptables -w -t mangle -I OUTPUT -o "$netdev" -p udp --dport 4791 \
        -m length --length 128:65535 -m comment --comment "$comment" \
        -j MARK --set-mark "$mark"
      ;;
    clear)
      while iptables -w -t mangle -D OUTPUT -o "$netdev" -p udp \
        --dport 4791 -m length --length 128:65535 -m comment \
        --comment "$comment" -j MARK --set-mark "$mark" 2>/dev/null; do :; done
      tc qdisc del dev "$netdev" root 2>/dev/null || true
      ;;
    *) fail "unknown RDMA delay action: $action" ;;
  esac
}

eviction_io() {
  local action=$1 unit=infiniswap-vm-eviction-io
  local output=$artifacts/fio-atomic-eviction.json result

  case $action in
    start)
      systemctl stop "$unit.service" 2>/dev/null || true
      systemctl reset-failed "$unit.service" 2>/dev/null || true
      rm -f "$output"
      systemd-run --quiet --unit="$unit" --service-type=exec -- \
        fio --name=atomic-eviction --filename="$device" --direct=1 \
          --ioengine=libaio --rw=randwrite --bs=4k --iodepth=32 --size=64m \
          --time_based=1 --runtime=45 --verify=crc32c --do_verify=1 \
          --verify_fatal=1 --group_reporting --output-format=json \
          --output="$output"
      ;;
    wait)
      for _ in {1..1200}; do
        systemctl is-active --quiet "$unit.service" || break
        sleep 0.1
      done
      systemctl is-active --quiet "$unit.service" &&
        fail "atomic eviction I/O did not finish"
      result=$(systemctl show -p Result --value "$unit.service")
      [[ $result == success ]] || fail "atomic eviction I/O result was $result"
      python3 - "$output" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    output = source.read()
start = output.find("{")
if start < 0:
    raise SystemExit("atomic eviction fio output is not JSON")
report = json.loads(output[start:])
if not report.get("jobs") or any(job.get("error", 0) for job in report["jobs"]):
    raise SystemExit("atomic eviction fio reported an I/O error")
PY
      systemctl reset-failed "$unit.service" 2>/dev/null || true
      ;;
    stop)
      systemctl stop "$unit.service" 2>/dev/null || true
      systemctl reset-failed "$unit.service" 2>/dev/null || true
      ;;
    *) fail "unknown eviction I/O action: $action" ;;
  esac
}

observe_eviction() {
  local action=$1 provider_id=${2:-} unit=infiniswap-vm-eviction-observer
  local script=/var/tmp/infiniswap-vm-observe-eviction
  local output=$artifacts/atomic-eviction.json result

  case $action in
    start)
      [[ -n $provider_id ]] || fail "eviction observer requires a Provider ID"
      systemctl stop "$unit.service" 2>/dev/null || true
      systemctl reset-failed "$unit.service" 2>/dev/null || true
      rm -f "$output"
      cat >"$script" <<'SH'
#!/usr/bin/env bash
set -euo pipefail

group=$1
output=$2
provider_id=$3
device=$4
started=$(date +%s%3N)
deadline=$((started + 60000))
local_before=$(<"$group/local_only_writes_total")
accepted_io=false
admission_closed=false
assignment_retained=false
fallback_submitted_while_draining=false
fallback_observed=false
release_complete=false
admission_ms=-1
release_ms=-1

while (($(date +%s%3N) <= deadline)); do
  now=$(date +%s%3N)
  mapped=$(<"$group/mapped_remote_chunks")
  placement=$(<"$group/remote_chunk_placements")
  local_after=$(<"$group/local_only_writes_total")
  inflight=$(<"$group/inflight_io")
  ((inflight > 0)) && accepted_io=true
  if ((mapped == 0)); then
    if [[ $admission_closed == false ]]; then
      admission_closed=true
      admission_ms=$((now - started))
    fi
    [[ $placement == *"0:$provider_id"* ]] && assignment_retained=true
    if [[ $placement == *"0:$provider_id"* &&
          $fallback_submitted_while_draining == false ]]; then
      timeout 10 dd if=/dev/zero of="$device" bs=4096 seek=200000 \
        count=1 oflag=direct conv=notrunc status=none
      fallback_submitted_while_draining=true
    fi
  fi
  ((local_after > local_before)) && fallback_observed=true
  if [[ $admission_closed == true && $placement == *"0:unmapped"* ]]; then
    release_complete=true
    release_ms=$((now - started))
  fi
  if [[ $accepted_io == true && $admission_closed == true &&
        $assignment_retained == true &&
        $fallback_submitted_while_draining == true &&
        $fallback_observed == true && $release_complete == true ]]; then
    printf '{"schema_version":1,"kind":"infiniswap.atomic-eviction",' >"$output"
    printf '"status":"passed","accepted_io_observed":true,' >>"$output"
    printf '"admission_closed":true,"assignment_retained_while_draining":true,' >>"$output"
    printf '"fallback_submitted_while_draining":true,' >>"$output"
    printf '"fallback_writes":%s,' \
      "$((local_after - local_before))" >>"$output"
    printf '"release_complete":true,' >>"$output"
    printf '"admission_ms":%s,"release_ms":%s}\n' \
      "$admission_ms" "$release_ms" >>"$output"
    exit 0
  fi
  sleep 0.005
done
printf 'eviction evidence incomplete: accepted=%s admission=%s retained=%s fallback_submitted=%s fallback_complete=%s release=%s\n' \
  "$accepted_io" "$admission_closed" "$assignment_retained" \
  "$fallback_submitted_while_draining" "$fallback_observed" \
  "$release_complete" >&2
exit 1
SH
      chmod 0755 "$script"
      systemd-run --quiet --unit="$unit" --service-type=exec -- \
        "$script" "$group" "$output" "$provider_id" "$device"
      ;;
    wait)
      for _ in {1..700}; do
        systemctl is-active --quiet "$unit.service" || break
        sleep 0.1
      done
      systemctl is-active --quiet "$unit.service" &&
        fail "eviction observer did not finish"
      result=$(systemctl show -p Result --value "$unit.service")
      if [[ $result != success ]]; then
        journalctl -u "$unit.service" --no-pager -n 50 >&2 || true
        fail "eviction observer result was $result"
      fi
      python3 - "$output" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="ascii") as source:
    report = json.load(source)
required = (
    "accepted_io_observed",
    "admission_closed",
    "assignment_retained_while_draining",
    "fallback_submitted_while_draining",
    "release_complete",
)
if report.get("status") != "passed" or not all(report.get(key) for key in required):
    raise SystemExit("atomic eviction evidence is incomplete")
if report.get("fallback_writes", 0) < 1:
    raise SystemExit("atomic eviction did not observe Backing Store fallback")
PY
      systemctl reset-failed "$unit.service" 2>/dev/null || true
      ;;
    stop)
      systemctl stop "$unit.service" 2>/dev/null || true
      systemctl reset-failed "$unit.service" 2>/dev/null || true
      ;;
    *) fail "unknown eviction observer action: $action" ;;
  esac
}

provider_pressure() {
  local action=$1 label=${2:-current} unit=infiniswap-vm-provider-pressure
  local script=/var/tmp/infiniswap-vm-provider-pressure.py
  local ready=/var/tmp/infiniswap-vm-provider-pressure.ready
  local status=$artifacts/provider-pressure-$label.json result

  case $action in
    start)
      systemctl stop "$unit.service" 2>/dev/null || true
      systemctl reset-failed "$unit.service" 2>/dev/null || true
      rm -f "$ready" "$status"
      cat >"$script" <<'PY'
import json
import re
import sys
import time
from pathlib import Path

ready = Path(sys.argv[1])
status = Path(sys.argv[2])
configuration = Path("/etc/infiniswap-vm-memory.conf").read_text(encoding="ascii")
match = re.search(r"^host_reserve_gib = ([0-9]+)$", configuration, re.MULTILINE)
if not match:
    raise SystemExit("Provider Host Reserve is absent")
reserve_gib = int(match.group(1))
target = max(512 * 1024 * 1024, reserve_gib * 1024**3 - 256 * 1024 * 1024)
chunk_size = 64 * 1024 * 1024
allocations = []
started = time.monotonic()

try:
    Path("/proc/self/oom_score_adj").write_text("1000\n", encoding="ascii")
except OSError:
    pass

def available_bytes():
    with open("/proc/meminfo", encoding="ascii") as source:
        for line in source:
            if line.startswith("MemAvailable:"):
                return int(line.split()[1]) * 1024
    raise RuntimeError("MemAvailable is absent")

while available_bytes() >= target:
    if time.monotonic() - started > 120:
        raise SystemExit("Provider pressure did not reach its target")
    allocation = bytearray(chunk_size)
    for offset in range(0, chunk_size, 4096):
        allocation[offset] = 0x5A
    allocations.append(allocation)
    time.sleep(0.01)

payload = {
    "schema_version": 1,
    "kind": "infiniswap.provider-pressure",
    "status": "target-reached",
    "allocated_bytes": len(allocations) * chunk_size,
    "mem_available_bytes": available_bytes(),
    "target_bytes": target,
    "host_reserve_gib": reserve_gib,
}
status.write_text(json.dumps(payload, sort_keys=True) + "\n", encoding="ascii")
ready.write_text("ready\n", encoding="ascii")
while True:
    time.sleep(1)
PY
      systemd-run --quiet --unit="$unit" --service-type=exec -- \
        python3 "$script" "$ready" "$status"
      for _ in {1..1300}; do
        [[ -f $ready ]] && return 0
        systemctl is-active --quiet "$unit.service" || break
        sleep 0.1
      done
      result=$(systemctl show -p Result --value "$unit.service")
      journalctl -u "$unit.service" --no-pager -n 50 >&2 || true
      fail "Provider pressure did not start (result $result)"
      ;;
    stop)
      systemctl stop "$unit.service" 2>/dev/null || true
      systemctl reset-failed "$unit.service" 2>/dev/null || true
      rm -f "$ready"
      ;;
    *) fail "unknown Provider pressure action: $action" ;;
  esac
}

verify_remote_only_committed() {
  local provider_id=$1 seconds=$2 started now placement

  [[ $seconds =~ ^[1-9][0-9]*$ ]] || fail "invalid committed-check duration"
  started=$(date +%s)
  while true; do
    [[ $(<"$group/mapped_remote_chunks") == 1 ]] ||
      fail "Provider pressure evicted a Committed Remote Chunk"
    [[ $(<"$group/remote_capacity_bytes") == "$chunk_bytes" ]] ||
      fail "Provider pressure changed committed capacity"
    placement=$(<"$group/remote_chunk_placements")
    [[ $placement == *"0:$provider_id"* ]] ||
      fail "Committed Remote Chunk lost its Provider assignment: $placement"
    [[ $(<"$group/connection_state") == connected ]] ||
      fail "Remote-Only connection changed under Provider pressure"
    now=$(date +%s)
    ((now - started >= seconds)) && break
    sleep 0.05
  done
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

run_fio_performance() {
  local mode=$1 phase=$2 sample=$3 duration=$4 rw=randrw do_verify=1
  local label="performance-$mode-$phase-$sample"
  local output="$artifacts/fio-$label.json"
  local verification_output=
  local -a args verification_args=()
  if [[ $mode == remote-only ]]; then
    rw=write
    do_verify=0
    verification_output="$artifacts/fio-$label-verify.json"
  fi
  args=(--name="$label" --filename="$device" --direct=1 --ioengine=libaio
    --rw="$rw" --bs=4k --iodepth=32 --size=64m
    --verify=crc32c --do_verify="$do_verify" --verify_fatal=1 --group_reporting
    --time_based=1 --runtime="$duration" --output-format=json --output="$output")
  if [[ $rw == randrw ]]; then
    args+=(--rwmixread=60)
  fi
  timeout "$((duration + 300))" fio "${args[@]}"
  if [[ $mode == remote-only ]]; then
    verification_args=(--name="$label-verify" --filename="$device" --direct=1
      --ioengine=libaio --rw=read --bs=4k --iodepth=32 --size=64m
      --verify=crc32c --verify_only=1 --verify_fatal=1 --group_reporting
      --output-format=json --output="$verification_output")
    timeout 300 fio "${verification_args[@]}"
  fi
  python3 - "$output" "$verification_output" "${args[@]}" \
    --verification-arguments "${verification_args[@]}" <<'PY'
import json
import sys


def load_fio(path):
    if not path:
        return None
    with open(path, encoding="utf-8") as source:
        output = source.read()
    start = output.find("{")
    if start < 0:
        raise SystemExit("fio output does not contain JSON")
    return json.loads(output[start:])


marker = sys.argv.index("--verification-arguments")
json.dump(
    {
        "arguments": sys.argv[3:marker],
        "fio": load_fio(sys.argv[1]),
        "verification": load_fio(sys.argv[2]),
        "verification_arguments": sys.argv[marker + 1 :],
    },
    sys.stdout,
    sort_keys=True,
)
sys.stdout.write("\n")
PY
}

performance_evidence() {
  local mode=$1 timeouts late errors inflight oldest
  check_kernel
  timeouts=$(<"$group/provider_timeouts_total")
  late=$(<"$group/late_rdma_completions_total")
  errors=$(<"$group/io_errors_total")
  inflight=$(<"$group/inflight_io")
  oldest=$(<"$group/oldest_inflight_ms")
  python3 - "$mode" "$timeouts" "$late" "$errors" "$inflight" "$oldest" <<'PY'
import json
import sys

mode, timeouts, late, errors, inflight, oldest = sys.argv[1:]
values = {
    "provider_timeouts_total": int(timeouts),
    "late_rdma_completions_total": int(late),
    "io_errors_total": int(errors),
    "inflight_io": int(inflight),
    "oldest_inflight_ms": int(oldest),
}
failed = any(
    values[field]
    for field in (
        "provider_timeouts_total",
        "late_rdma_completions_total",
        "inflight_io",
        "oldest_inflight_ms",
    )
)
json.dump(
    {
        "status": "failed" if failed else "passed",
        "mode": mode,
        **values,
    },
    sys.stdout,
    sort_keys=True,
)
sys.stdout.write("\n")
if failed:
    raise SystemExit("performance run observed timeout, late completion, or hung I/O")
PY
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
        late_rdma_completions_total local_only_writes_total \
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
  systemctl stop infiniswap-vm-eviction-io.service \
    infiniswap-vm-eviction-observer.service \
    infiniswap-vm-provider-pressure.service 2>/dev/null || true
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
  fio-performance) run_fio_performance "$@" ;;
  performance-evidence) performance_evidence "$@" ;;
  verify-remote-only-heartbeats) verify_remote_only_heartbeats "$@" ;;
  remote-chunk-contracts) remote_chunk_contracts ;;
  rdma-delay) rdma_delay "$@" ;;
  eviction-io) eviction_io "$@" ;;
  observe-eviction) observe_eviction "$@" ;;
  provider-pressure) provider_pressure "$@" ;;
  verify-remote-only-committed) verify_remote_only_committed "$@" ;;
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
