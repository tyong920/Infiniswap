#!/usr/bin/env bash
set -euo pipefail

# Destructive single-host Soft-RoCE certification for the first remote path.
# The supplied Backing Store is overwritten across its first 2 GiB.
if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "run as root" >&2
  exit 2
fi
if [[ ${INFINISWAP_TEST_DESTRUCTIVE:-} != yes ]]; then
  echo "set INFINISWAP_TEST_DESTRUCTIVE=yes" >&2
  exit 2
fi

backing=${INFINISWAP_TEST_BACKING:-}
module=${INFINISWAP_TEST_MODULE:-}
provider=${INFINISWAP_TEST_PROVIDER:-}
rail=${INFINISWAP_TEST_RDMA_DEVICE:-rxe0}
address=${INFINISWAP_TEST_RDMA_ADDRESS:-}
netdev=${INFINISWAP_TEST_RDMA_NETDEV:-}
port=${INFINISWAP_TEST_PROVIDER_PORT:-19400}
failure_deadline_ms=${INFINISWAP_TEST_FAILURE_DEADLINE_MS:-2000}
capacity_bytes=$((2 * 1024 * 1024 * 1024))
chunk_bytes=$((1024 * 1024 * 1024))
configfs=/sys/kernel/config
root=$configfs/infiniswap
tmp=$(mktemp -d)
provider_pid=
dm_delay_name=
loaded_module=0
mounted_configfs=0
network_fault_active=0
declare -a created_groups=()

fail() {
  echo "remote Backed Mode test failed: $*" >&2
  [[ -f $tmp/provider.log ]] && tail -100 "$tmp/provider.log" >&2
  exit 1
}

wait_for_value() {
  local path=$1
  local expected=$2
  local attempt

  for ((attempt = 0; attempt < 300; attempt++)); do
    if [[ -r $path && $(<"$path") == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  fail "$path did not become $expected"
}

wait_for_value_deadline() {
  local path=$1
  local expected=$2
  local deadline_ms=$3
  local started now

  started=$(date +%s%3N)
  while true; do
    if [[ -r $path && $(<"$path") == "$expected" ]]; then
      now=$(date +%s%3N)
      ((now - started <= deadline_ms)) || \
        fail "$path took $((now - started)) ms to become $expected"
      return 0
    fi
    now=$(date +%s%3N)
    ((now - started <= deadline_ms)) || \
      fail "$path did not become $expected within ${deadline_ms} ms"
    sleep 0.01
  done
}

wait_for_path() {
  local path=$1
  local expected=$2
  local attempt

  for ((attempt = 0; attempt < 200; attempt++)); do
    if [[ $expected == present && -b $path ]]; then
      return 0
    fi
    if [[ $expected == absent && ! -e $path ]]; then
      return 0
    fi
    sleep 0.05
  done
  fail "$path did not become $expected"
}

measure_parallel_write_p99() {
  local device=$1
  local pattern=$2
  local prefix=$3
  local sample started_ms elapsed_ms result fault_pid fault_status=0
  local -a pids=()
  local -a latencies=()

  set +e
  for sample in {0..99}; do
    (
      started_ms=$(date +%s%3N)
      timeout 5 dd if="$pattern" of="$device" bs=4096 skip="$sample" \
        seek="$sample" count=1 oflag=direct conv=notrunc status=none
      result=$?
      elapsed_ms=$(($(date +%s%3N) - started_ms))
      printf '%s %s\n' "$elapsed_ms" "$result" > "$prefix-$sample"
      exit "$result"
    ) &
    pids+=("$!")
  done
  for fault_pid in "${pids[@]}"; do
    wait "$fault_pid" || fault_status=1
  done
  set -e
  ((fault_status == 0)) || \
    fail "failure fallback returned an application-visible write error"
  mapfile -t latencies < <(
    awk '{if ($2 != 0) exit 1; print $1}' "$prefix"-* | sort -n
  )
  [[ ${#latencies[@]} == 100 ]] || \
    fail "failure fallback did not produce 100 latency samples"
  measured_p99_ms=${latencies[98]}
}

stop_group() {
  local name=$1
  local group=$root/$name
  local attempt

  [[ -d $group ]] || return 0
  for ((attempt = 0; attempt < 200; attempt++)); do
    if printf 'stop\n' > "$group/state" 2>/dev/null; then
      break
    fi
    sleep 0.05
  done
  [[ $(<"$group/state") == stopped ]] || return 1
  wait_for_path "/dev/$name" absent
  rmdir "$group"
}

cleanup() {
  local index

  set +e
  if ((network_fault_active)) && [[ -n $netdev ]]; then
    tc qdisc del dev "$netdev" root 2>/dev/null
  fi
  for ((index = ${#created_groups[@]} - 1; index >= 0; index--)); do
    stop_group "${created_groups[index]}"
  done
  if [[ -n $provider_pid ]]; then
    kill -TERM "$provider_pid" 2>/dev/null
    wait "$provider_pid" 2>/dev/null
  fi
  if ((loaded_module)); then
    rmmod infiniswap
  fi
  if [[ -n $dm_delay_name ]]; then
    dmsetup remove "$dm_delay_name"
  fi
  if ((mounted_configfs)); then
    umount "$configfs"
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

for command in awk blockdev cmp date dd dmesg dmsetup fio grep insmod kill modprobe \
  mount mountpoint openssl rdma readlink rmmod sort tail tc timeout; do
  command -v "$command" >/dev/null || fail "missing command: $command"
done
[[ -n $backing && -n $provider && -n $address ]] || \
  fail "set INFINISWAP_TEST_BACKING, INFINISWAP_TEST_PROVIDER, and INFINISWAP_TEST_RDMA_ADDRESS"
[[ -x $provider ]] || fail "Provider executable is not executable: $provider"
[[ -f $module ]] || fail "Memory Consumer module does not exist: $module"
rdma link show "$rail/1" >/dev/null 2>&1 || fail "RDMA Rail is unavailable: $rail"
if [[ -z $netdev ]]; then
  netdev=$(rdma link show "$rail/1" | awk '{for (i=1; i<=NF; i++) if ($i == "netdev") print $(i+1)}')
fi
[[ -n $netdev ]] || fail "could not resolve the netdev for $rail/1"
[[ -d /sys/class/infiniband/$rail/ports/1 ]] || fail "$rail port 1 is unavailable"
numa_path="/sys/class/infiniband/$rail/device/numa_node"
if [[ -r $numa_path ]]; then
  numa_node=$(<"$numa_path")
else
  numa_node=-1
fi

backing=$(readlink -f "$backing")
[[ -b $backing ]] || fail "Backing Store must be a block device"
(( $(blockdev --getsize64 "$backing") >= capacity_bytes )) || \
  fail "Backing Store must provide at least 2 GiB"
raw_backing=$backing
modprobe dm_delay
dm_delay_name="infiniswap-delay-$$"
printf '0 %s delay %s 0 0 %s 0 200\n' \
  "$((capacity_bytes / 512))" "$raw_backing" "$raw_backing" | \
  dmsetup create "$dm_delay_name"
backing="/dev/mapper/$dm_delay_name"
[[ -b $backing ]] || fail "could not create the delayed Backing Store"
if [[ -d /sys/module/infiniswap ]]; then
  fail "infiniswap is already loaded; use a dedicated disposable VM"
fi

dmesg_start=$(dmesg | wc -l)
psk_hex=$(openssl rand -hex 32)
cat > "$tmp/provider-memory.conf" <<EOF
version = 1
host_reserve_gib = 1
max_opportunistic_gib = 1
max_committed_gib = 0
EOF
cat > "$tmp/consumers.conf" <<EOF
version = 1

[consumer:consumer-test]
current_key_id = key-test
current_psk_hex = $psk_hex
max_connections = 1
max_opportunistic_gib = 1
max_committed_gib = 0
revoked = false
EOF
chmod 0600 "$tmp/consumers.conf"
ulimit -l unlimited 2>/dev/null || fail "could not raise the Provider memlock limit"
"$provider" :: "$port" "$tmp/provider-memory.conf" \
  "$tmp/consumers.conf" >"$tmp/provider.log" 2>&1 &
provider_pid=$!
sleep 1
kill -0 "$provider_pid" 2>/dev/null || fail "Provider did not start"

if ! mountpoint -q "$configfs"; then
  mount -t configfs none "$configfs"
  mounted_configfs=1
fi
insmod "$module"
loaded_module=1
[[ -d $root ]] || fail "configfs subsystem was not registered"

configure_group() {
  local name=$1
  local secret=$2
  local policy=${3:-strict}
  local group=$root/$name

  mkdir "$group"
  created_groups+=("$name")
  printf 'backed\n' > "$group/mode"
  printf '%s\n' "$policy" > "$group/acknowledgement_policy"
  printf '%s\n' "$backing" > "$group/backing_store"
  printf '%s\n' "$capacity_bytes" > "$group/capacity_bytes"
  printf '%s\n' "$failure_deadline_ms" > "$group/provider_failure_deadline_ms"
  printf '1\n' > "$group/hot_range_threshold"
  printf '1\n' > "$group/hot_range_read_weight"
  printf '4\n' > "$group/hot_range_write_weight"
  printf 'consumer-test\n' > "$group/consumer_id"
  printf 'provider-test\n' > "$group/providers"
  printf '%s\n' "$address" > "$group/provider_address"
  printf '%s\n' "$port" > "$group/provider_port"
  printf '%s\n' "$rail" > "$group/rdma_device"
  printf '1\n' > "$group/rdma_port"
  printf '%s\n' "$numa_node" > "$group/rdma_numa_node"
  printf 'key-test\n' > "$group/provider_key_id"
  printf '%s\n' "$secret" > "$group/provider_psk"
  printf '100\n' > "$group/swap_priority"
  printf 'activate\n' > "$group/state"
  wait_for_path "/dev/$name" present
}

# A rejected authenticated setup remains a correct local Backed Mode device.
bad_psk=$(openssl rand -hex 32)
configure_group infiniswap-rejected "$bad_psk"
wait_for_value "$root/infiniswap-rejected/connection_state" not-connected
dd if=/dev/urandom of="$tmp/rejected-pattern" bs=4096 count=16 status=none
dd if="$tmp/rejected-pattern" of=/dev/infiniswap-rejected bs=4096 count=16 \
  oflag=direct conv=fsync status=none
dd if=/dev/infiniswap-rejected of="$tmp/rejected-actual" bs=4096 count=16 \
  iflag=direct status=none
cmp "$tmp/rejected-pattern" "$tmp/rejected-actual" || \
  fail "rejected setup corrupted local I/O"
stop_group infiniswap-rejected || fail "rejected session did not tear down"
sleep 1

configure_group infiniswap-remote "$psk_hex"
wait_for_value "$root/infiniswap-remote/connection_state" connected

# The first write is local and crosses the runtime threshold; the next I/O uses
# the resulting one-GiB Remote Chunk.
dd if=/dev/urandom of="$tmp/hot-seed" bs=4096 count=1 status=none
dd if="$tmp/hot-seed" of=/dev/infiniswap-remote bs=4096 count=1 \
  oflag=direct conv=fsync status=none
wait_for_value "$root/infiniswap-remote/mapped_hot_ranges" 1
[[ $(<"$root/infiniswap-remote/remote_capacity_bytes") == "$chunk_bytes" ]] || \
  fail "mapped capacity is not exactly one Remote Chunk"

# Heating a second range beyond the Provider's one-chunk quota must stay local
# without turning a retryable admission limit into a connection failure.
dd if=/dev/urandom of="$tmp/quota-pattern" bs=4096 count=16 status=none
dd if="$tmp/quota-pattern" of=/dev/infiniswap-remote bs=4096 count=16 \
  seek=$((1536 * 1024 / 4)) oflag=direct conv=fsync status=none
dd if=/dev/infiniswap-remote of="$tmp/quota-actual" bs=4096 count=16 \
  skip=$((1536 * 1024 / 4)) iflag=direct status=none
cmp "$tmp/quota-pattern" "$tmp/quota-actual" || \
  fail "quota-limited local data was corrupted"
wait_for_value "$root/infiniswap-remote/connection_state" connected
[[ $(<"$root/infiniswap-remote/mapped_hot_ranges") == 1 ]] || \
  fail "Consumer exceeded the Provider's advertised Remote Chunk budget"

fio --name=remote-random --filename=/dev/infiniswap-remote --direct=1 \
  --ioengine=libaio --rw=randrw --rwmixread=60 --bs=4k --iodepth=32 \
  --size=64m --verify=crc32c --do_verify=1 --verify_fatal=1 --group_reporting
wait_for_value "$root/infiniswap-remote/connection_state" connected
[[ $(<"$root/infiniswap-remote/mapped_hot_ranges") == 1 ]] || \
  fail "verified I/O lost its Remote Chunk"

# Raising the threshold at runtime leaves the second Hot Range cold and proves
# unmapped reads continue to use the recoverable Backing Store.
printf '1000000000\n' > "$root/infiniswap-remote/hot_range_threshold"
dd if=/dev/urandom of="$tmp/cold-pattern" bs=4096 count=16 status=none
dd if="$tmp/cold-pattern" of=/dev/infiniswap-remote bs=4096 count=16 \
  seek=$((1536 * 1024 / 4)) oflag=direct conv=fsync status=none
dd if=/dev/infiniswap-remote of="$tmp/cold-actual" bs=4096 count=16 \
  skip=$((1536 * 1024 / 4)) iflag=direct status=none
cmp "$tmp/cold-pattern" "$tmp/cold-actual" || fail "cold local data was corrupted"
[[ $(<"$root/infiniswap-remote/mapped_hot_ranges") == 1 ]] || \
  fail "runtime threshold did not keep the cold range unmapped"

stop_group infiniswap-remote || fail "remote session did not tear down"
sleep 1

# Repeated authenticated setup and teardown catches stale QPs, CQs, MRs, work,
# and request contexts.
for iteration in 1 2 3; do
  name="infiniswap-repeat-$iteration"
  configure_group "$name" "$psk_hex"
  wait_for_value "$root/$name/connection_state" connected
  dd if=/dev/zero of="/dev/$name" bs=4096 count=1 oflag=direct status=none
  wait_for_value "$root/$name/mapped_hot_ranges" 1
  stop_group "$name" || fail "$name did not tear down"
  sleep 1
done

# Remote-First retries one delayed Backing Store failure, then enters the
# operator-visible Backing-Degraded state and rejects every later write.
configure_group infiniswap-backing-degraded "$psk_hex" remote-first
wait_for_value "$root/infiniswap-backing-degraded/connection_state" connected
dd if=/dev/urandom of="$tmp/degraded-seed" bs=4096 count=1 status=none
dd if="$tmp/degraded-seed" of=/dev/infiniswap-backing-degraded bs=4096 count=1 \
  oflag=direct conv=fsync status=none
wait_for_value "$root/infiniswap-backing-degraded/mapped_hot_ranges" 1
dmsetup suspend "$dm_delay_name"
printf '0 %s error\n' "$((capacity_bytes / 512))" | \
  dmsetup reload "$dm_delay_name"
dmsetup resume "$dm_delay_name"
dd if=/dev/urandom of="$tmp/degraded-pattern" bs=4096 count=1 status=none
timeout 5 dd if="$tmp/degraded-pattern" of=/dev/infiniswap-backing-degraded \
  bs=4096 count=1 oflag=direct status=none || \
  fail "Remote-First did not preserve its completed remote write"
wait_for_value "$root/infiniswap-backing-degraded/backing_state" backing-degraded
[[ $(<"$root/infiniswap-backing-degraded/backing_retries_total") == 1 ]] || \
  fail "Remote-First did not retry the delayed backing failure exactly once"
[[ $(<"$root/infiniswap-backing-degraded/backing_degraded_transitions_total") == 1 ]] || \
  fail "Backing-Degraded transition metric was not recorded"
if timeout 5 dd if=/dev/zero of=/dev/infiniswap-backing-degraded bs=4096 \
  count=1 oflag=direct status=none; then
  fail "Backing-Degraded accepted a new write"
fi
[[ $(<"$root/infiniswap-backing-degraded/rejected_writes_total") -ge 1 ]] || \
  fail "rejected Backing-Degraded write was not counted"
dmsetup suspend "$dm_delay_name"
printf '0 %s delay %s 0 0 %s 0 200\n' \
  "$((capacity_bytes / 512))" "$raw_backing" "$raw_backing" | \
  dmsetup reload "$dm_delay_name"
dmsetup resume "$dm_delay_name"
printf 'stop\n' > "$root/infiniswap-backing-degraded/state"
wait_for_value "$root/infiniswap-backing-degraded/state" stopped
if printf 'activate\n' > "$root/infiniswap-backing-degraded/state" 2>/dev/null; then
  fail "Backing-Degraded was silently reset without device recreation"
fi
rmdir "$root/infiniswap-backing-degraded"

# A silent network interruption exercises the per-operation watchdog. The
# timed-out Strict write succeeds from its valid Backing Store copy, and the
# late RDMA completion cannot overwrite the later Backing Store read.
configure_group infiniswap-network-fault "$psk_hex" strict
wait_for_value "$root/infiniswap-network-fault/connection_state" connected
dd if=/dev/urandom of="$tmp/network-seed" bs=4096 count=1 status=none
dd if="$tmp/network-seed" of=/dev/infiniswap-network-fault bs=4096 count=1 \
  oflag=direct conv=fsync status=none
wait_for_value "$root/infiniswap-network-fault/mapped_hot_ranges" 1
dd if=/dev/urandom of="$tmp/network-pattern" bs=4096 count=100 status=none
tc qdisc replace dev "$netdev" root netem loss 100%
network_fault_active=1
measure_parallel_write_p99 /dev/infiniswap-network-fault \
  "$tmp/network-pattern" "$tmp/network-latency"
((measured_p99_ms <= failure_deadline_ms + 500)) || \
  fail "network fallback p99 was ${measured_p99_ms} ms"
echo "network fallback p99: ${measured_p99_ms} ms"
wait_for_value "$root/infiniswap-network-fault/connection_state" degraded
tc qdisc del dev "$netdev" root
network_fault_active=0
dd if=/dev/infiniswap-network-fault of="$tmp/network-actual" bs=4096 \
  count=100 iflag=direct status=none
cmp "$tmp/network-pattern" "$tmp/network-actual" || \
  fail "late RDMA completion caused a stale read"
[[ $(<"$root/infiniswap-network-fault/provider_timeouts_total") -ge 1 ]] || \
  fail "Provider timeout was not counted"
[[ $(<"$root/infiniswap-network-fault/late_rdma_completions_total") -ge 1 ]] || \
  fail "late RDMA completion was not ignored and counted"
stop_group infiniswap-network-fault || fail "network fault device did not tear down"
sleep 1

# Provider process death transitions promptly and leaves the valid Backing
# Store available without an application-visible read error.
configure_group infiniswap-provider-death "$psk_hex" strict
wait_for_value "$root/infiniswap-provider-death/connection_state" connected
dd if=/dev/urandom of="$tmp/death-pattern" bs=4096 count=1 status=none
dd if="$tmp/death-pattern" of=/dev/infiniswap-provider-death bs=4096 count=1 \
  oflag=direct conv=fsync status=none
wait_for_value "$root/infiniswap-provider-death/mapped_hot_ranges" 1
kill -KILL "$provider_pid"
wait "$provider_pid" 2>/dev/null || true
provider_pid=
wait_for_value_deadline "$root/infiniswap-provider-death/connection_state" \
  degraded "$((failure_deadline_ms + 250))"
dd if=/dev/urandom of="$tmp/death-pattern" bs=4096 count=100 status=none
measure_parallel_write_p99 /dev/infiniswap-provider-death \
  "$tmp/death-pattern" "$tmp/death-latency"
((measured_p99_ms <= failure_deadline_ms + 500)) || \
  fail "Provider-death fallback p99 was ${measured_p99_ms} ms"
echo "Provider-death fallback p99: ${measured_p99_ms} ms"
dd if=/dev/infiniswap-provider-death of="$tmp/death-actual" bs=4096 \
  count=100 iflag=direct status=none
cmp "$tmp/death-pattern" "$tmp/death-actual" || \
  fail "Provider death did not preserve Backing Store data"
stop_group infiniswap-provider-death || fail "Provider death device did not tear down"

dmesg | tail -n "+$((dmesg_start + 1))" > "$tmp/kernel.log"
if grep -Eiq 'BUG:|WARNING:|Oops:|kernel panic|use-after-free|refcount.*underflow' \
  "$tmp/kernel.log"; then
  cat "$tmp/kernel.log" >&2
  fail "kernel diagnostics reported a correctness failure"
fi

echo "single-Provider Backed Strict/Remote-First failure verification passed"
