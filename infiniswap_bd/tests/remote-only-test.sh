#!/usr/bin/env bash
set -euo pipefail

# Destructive Soft-RoCE verification for Remote-Only Mode.
if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "run as root" >&2
  exit 2
fi
if [[ ${INFINISWAP_TEST_DESTRUCTIVE:-} != yes ]]; then
  echo "set INFINISWAP_TEST_DESTRUCTIVE=yes" >&2
  exit 2
fi

module=${INFINISWAP_TEST_MODULE:-}
provider=${INFINISWAP_TEST_PROVIDER:-}
external_provider=${INFINISWAP_TEST_EXTERNAL_PROVIDER:-no}
test_case=${INFINISWAP_TEST_CASE:-all}
fault_mode=${INFINISWAP_TEST_FAULT_MODE:-netem}
rail=${INFINISWAP_TEST_RDMA_DEVICE:-rxe0}
address=${INFINISWAP_TEST_RDMA_ADDRESS:-}
netdev=${INFINISWAP_TEST_RDMA_NETDEV:-}
port=${INFINISWAP_TEST_PROVIDER_PORT:-19401}
failure_deadline_ms=${INFINISWAP_TEST_FAILURE_DEADLINE_MS:-2000}
fault_tag="infiniswap-ro-test-$$"
chunk_bytes=$((1024 * 1024 * 1024))
capacity_bytes=$((2 * chunk_bytes))
configfs=/sys/kernel/config
root=$configfs/infiniswap
tmp=$(mktemp -d)
provider_pid=
loaded_module=0
mounted_configfs=0
network_fault_active=0
declare -a created_groups=()

fail() {
  echo "Remote-Only Mode test failed: $*" >&2
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

wait_for_counter_at_least_deadline() {
  local path=$1
  local minimum=$2
  local started_ms=$3
  local deadline_ms=$4
  local now value

  while true; do
    value=$(<"$path")
    if ((value >= minimum)); then
      return 0
    fi
    now=$(date +%s%3N)
    ((now - started_ms <= deadline_ms)) ||
      fail "network fault injection did not produce a Provider timeout"
    sleep 0.01
  done
}

wait_for_remote_lost_deadline() {
  local connection_path=$1
  local operational_path=$2
  local deadline_ms=$3
  local started now

  started=$(date +%s%3N)
  while true; do
    if [[ -r $connection_path && -r $operational_path &&
          $(<"$connection_path") == remote-lost &&
          $(<"$operational_path") == remote-lost ]]; then
      now=$(date +%s%3N)
      ((now - started <= deadline_ms)) ||
        fail "Remote-Lost took $((now - started)) ms"
      return 0
    fi
    now=$(date +%s%3N)
    ((now - started <= deadline_ms)) ||
      fail "Remote-Lost did not become terminal within ${deadline_ms} ms"
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

expect_explicit_io_failure() {
  local description=$1
  local status

  shift
  if timeout 5 "$@"; then
    fail "Remote-Lost accepted a $description"
  else
    status=$?
  fi
  [[ $status -eq 1 ]] ||
    fail "Remote-Lost $description did not fail explicitly (status $status)"
}

clear_network_fault() {
  case $fault_mode in
    netem)
      tc qdisc del dev "$netdev" root 2>/dev/null
      ;;
    roce-iptables)
      iptables -w -D OUTPUT -p udp -d "$address" --dport 4791 \
        -m comment --comment "$fault_tag" -j DROP 2>/dev/null
      ;;
  esac
}

inject_network_fault() {
  case $fault_mode in
    netem)
      tc qdisc replace dev "$netdev" root netem loss 100%
      ;;
    roce-iptables)
      iptables -w -I OUTPUT -p udp -d "$address" --dport 4791 \
        -m comment --comment "$fault_tag" -j DROP
      ;;
  esac
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
  if ((network_fault_active)); then
    clear_network_fault
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
  if ((mounted_configfs)); then
    umount "$configfs"
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

for command in awk cmp date dd dmesg grep insmod kill mount mountpoint openssl \
  rdma rmmod tail timeout wc; do
  command -v "$command" >/dev/null || fail "missing command: $command"
done
[[ -n $module && -n $address ]] ||
  fail "set INFINISWAP_TEST_MODULE and INFINISWAP_TEST_RDMA_ADDRESS"
case $test_case in
  all | network-fault) ;;
  *) fail "INFINISWAP_TEST_CASE must be all or network-fault" ;;
esac
case $external_provider in
  yes | no) ;;
  *) fail "INFINISWAP_TEST_EXTERNAL_PROVIDER must be yes or no" ;;
esac
if [[ $test_case == network-fault && $external_provider != yes ]]; then
  fail "network-fault requires INFINISWAP_TEST_EXTERNAL_PROVIDER=yes"
fi
network_test_enabled=0
if [[ $test_case == network-fault || $external_provider == yes ]]; then
  network_test_enabled=1
fi
case $fault_mode in
  netem)
    if ((network_test_enabled)); then
      command -v tc >/dev/null || fail "missing command: tc"
    fi
    ;;
  roce-iptables)
    if ((network_test_enabled)); then
      command -v iptables >/dev/null || fail "missing command: iptables"
    fi
    [[ $address != *:* ]] ||
      fail "roce-iptables fault injection requires an IPv4 Provider address"
    ;;
  *) fail "INFINISWAP_TEST_FAULT_MODE must be netem or roce-iptables" ;;
esac
if [[ $external_provider == no ]]; then
  [[ -n $provider ]] || fail "set INFINISWAP_TEST_PROVIDER"
  [[ -x $provider ]] || fail "Memory Provider is not executable: $provider"
else
  [[ -n ${INFINISWAP_TEST_PSK_HEX:-} ]] ||
    fail "set INFINISWAP_TEST_PSK_HEX for an external Provider"
fi
[[ -f $module ]] || fail "Memory Consumer module does not exist: $module"
rdma link show "$rail/1" >/dev/null 2>&1 ||
  fail "RDMA Rail is unavailable: $rail"
if [[ -z $netdev ]]; then
  netdev=$(rdma link show "$rail/1" |
    awk '{for (i=1; i<=NF; i++) if ($i == "netdev") print $(i+1)}')
fi
[[ -n $netdev ]] || fail "could not resolve the netdev for $rail/1"
numa_path=/sys/class/infiniband/$rail/device/numa_node
if [[ -r $numa_path ]]; then
  numa_node=$(<"$numa_path")
else
  numa_node=-1
fi
if [[ -d /sys/module/infiniswap ]]; then
  fail "infiniswap is already loaded; use a dedicated disposable VM"
fi

psk_hex=${INFINISWAP_TEST_PSK_HEX:-}
if [[ -z $psk_hex ]]; then
  psk_hex=$(openssl rand -hex 32)
fi
[[ $psk_hex =~ ^[[:xdigit:]]{64}$ ]] ||
  fail "INFINISWAP_TEST_PSK_HEX must contain exactly 64 hexadecimal digits"
cat > "$tmp/provider-memory.conf" <<EOF
version = 1
host_reserve_gib = 1
max_opportunistic_gib = 0
max_committed_gib = 2
EOF
cat > "$tmp/consumers.conf" <<EOF
version = 1

[consumer:consumer-test]
current_key_id = key-test
current_psk_hex = $psk_hex
max_connections = 1
max_opportunistic_gib = 0
max_committed_gib = 2
revoked = false
EOF
chmod 0600 "$tmp/consumers.conf"
if [[ $external_provider == no ]]; then
  ulimit -l unlimited 2>/dev/null ||
    fail "could not raise the Provider memlock limit"
  "$provider" :: "$port" "$tmp/provider-memory.conf" \
    "$tmp/consumers.conf" >"$tmp/provider.log" 2>&1 &
  provider_pid=$!
  sleep 1
  kill -0 "$provider_pid" 2>/dev/null || fail "Provider did not start"
fi

if ! mountpoint -q "$configfs"; then
  mount -t configfs none "$configfs"
  mounted_configfs=1
fi
insmod "$module"
loaded_module=1
[[ -d $root ]] || fail "configfs subsystem was not registered"
dmesg_start=$(dmesg | wc -l)

configure_group() {
  local name=$1
  local bytes=$2
  local eligible=$3
  local group=$root/$name

  mkdir "$group"
  created_groups+=("$name")
  printf 'remote-only\n' > "$group/mode"
  printf '%s\n' "$eligible" > "$group/remote_only_eligible"
  printf '%s\n' "$bytes" > "$group/capacity_bytes"
  printf '%s\n' "$failure_deadline_ms" > "$group/provider_failure_deadline_ms"
  printf 'consumer-test\n' > "$group/consumer_id"
  printf 'provider-test\n' > "$group/providers"
  printf '%s\n' "$address" > "$group/provider_address"
  printf '%s\n' "$port" > "$group/provider_port"
  printf '%s\n' "$rail" > "$group/rdma_device"
  printf '1\n' > "$group/rdma_port"
  printf '%s\n' "$numa_node" > "$group/rdma_numa_node"
  printf 'key-test\n' > "$group/provider_key_id"
  printf '%s\n' "$psk_hex" > "$group/provider_psk"
  printf '100\n' > "$group/swap_priority"
}

test_network_fault() {
  local name=$1
  local group=$root/$name
  local fault_started now transition_budget transition_elapsed timeouts

  fault_started=$(date +%s%3N)
  network_fault_active=1
  inject_network_fault
  wait_for_counter_at_least_deadline \
    "$group/provider_timeouts_total" 1 "$fault_started" \
    "$((failure_deadline_ms + 500))"

  now=$(date +%s%3N)
  transition_budget=$((failure_deadline_ms + 500 - (now - fault_started)))
  ((transition_budget >= 0)) ||
    fail "Provider timeout exceeded the Remote-Lost transition deadline"
  wait_for_remote_lost_deadline \
    "$group/connection_state" "$group/operational_state" \
    "$transition_budget"
  transition_elapsed=$(($(date +%s%3N) - fault_started))

  clear_network_fault
  network_fault_active=0
  timeouts=$(<"$group/provider_timeouts_total")
  [[ $(<"$group/remote_lost_transitions_total") == 1 ]] ||
    fail "Remote-Lost transition was not counted exactly once"
  expect_explicit_io_failure write dd if=/dev/zero \
    of="/dev/$name" bs=4096 count=1 oflag=direct status=none
  expect_explicit_io_failure read dd if="/dev/$name" \
    of="$tmp/lost-read" bs=4096 count=1 iflag=direct status=none
  printf 'network fault elapsed_ms=%s state=%s timeouts=%s\n' \
    "$transition_elapsed" "$(<"$group/connection_state")" "$timeouts"
}

if [[ $test_case == network-fault ]]; then
  configure_group infiniswap-remote-only "$chunk_bytes" 1
  printf 'activate\n' > "$root/infiniswap-remote-only/state"
  wait_for_path /dev/infiniswap-remote-only present
  wait_for_value "$root/infiniswap-remote-only/connection_state" connected
  [[ $(<"$root/infiniswap-remote-only/mapped_remote_chunks") == 1 ]] ||
    fail "activation did not map its Remote Chunk"
  test_network_fault infiniswap-remote-only
  stop_group infiniswap-remote-only ||
    fail "Remote-Lost device did not stop"
  echo "focused Remote-Only network interruption verification passed"
  exit 0
fi

# Direct configfs callers cannot bypass the host-wide recoverability gate.
configure_group infiniswap-ineligible "$capacity_bytes" 0
if printf 'activate\n' > "$root/infiniswap-ineligible/state" 2>/dev/null; then
  fail "Remote-Only Mode activated without host eligibility"
fi
[[ ! -e /dev/infiniswap-ineligible ]] ||
  fail "ineligible activation exposed a block device"
stop_group infiniswap-ineligible || fail "ineligible configuration did not stop"

# A failed full-capacity admission exposes neither a partial device nor capacity.
configure_group infiniswap-insufficient "$((3 * chunk_bytes))" 1
if printf 'activate\n' > "$root/infiniswap-insufficient/state" 2>/dev/null; then
  fail "partial committed capacity activated a block device"
fi
[[ ! -e /dev/infiniswap-insufficient ]] ||
  fail "insufficient committed capacity exposed a block device"
[[ $(<"$root/infiniswap-insufficient/remote_capacity_bytes") == 0 ]] ||
  fail "failed activation retained partial Remote Memory"
stop_group infiniswap-insufficient || fail "failed admission did not stop"
sleep 1

# Repeated full reservations prove disconnect releases all committed accounting.
for iteration in 1 2 3; do
  name="infiniswap-repeat-$iteration"
  configure_group "$name" "$capacity_bytes" 1
  printf 'activate\n' > "$root/$name/state"
  wait_for_path "/dev/$name" present
  [[ $(<"$root/$name/mapped_remote_chunks") == 2 ]] ||
    fail "$name did not reserve every Remote Chunk"
  [[ $(<"$root/$name/remote_capacity_bytes") == "$capacity_bytes" ]] ||
    fail "$name exposed partial Remote Memory"
  stop_group "$name" || fail "$name did not release committed capacity"
  sleep 1
done

configure_group infiniswap-remote-only "$capacity_bytes" 1
printf 'activate\n' > "$root/infiniswap-remote-only/state"
wait_for_path /dev/infiniswap-remote-only present
wait_for_value "$root/infiniswap-remote-only/connection_state" connected
[[ $(<"$root/infiniswap-remote-only/mapped_remote_chunks") == 2 ]] ||
  fail "activation did not map the full advertised capacity"
[[ $(<"$root/infiniswap-remote-only/remote_capacity_bytes") == "$capacity_bytes" ]] ||
  fail "activation exposed partial committed capacity"

dd if=/dev/urandom of="$tmp/pattern" bs=4096 count=1024 status=none
dd if="$tmp/pattern" of=/dev/infiniswap-remote-only bs=4096 count=1024 \
  oflag=direct conv=fsync status=none
dd if=/dev/infiniswap-remote-only of="$tmp/actual" bs=4096 count=1024 \
  iflag=direct status=none
cmp "$tmp/pattern" "$tmp/actual" || fail "verified Remote-Only I/O mismatched"
dd if=/dev/urandom of="$tmp/boundary-pattern" bs=4096 count=16 status=none
dd if="$tmp/boundary-pattern" of=/dev/infiniswap-remote-only bs=4096 \
  seek=$((chunk_bytes / 4096 - 8)) count=16 oflag=direct conv=fsync status=none
dd if=/dev/infiniswap-remote-only of="$tmp/boundary-actual" bs=4096 \
  skip=$((chunk_bytes / 4096 - 8)) count=16 iflag=direct status=none
cmp "$tmp/boundary-pattern" "$tmp/boundary-actual" ||
  fail "Remote Chunk boundary I/O mismatched"

if [[ $external_provider == yes ]]; then
  test_network_fault infiniswap-remote-only
fi

stop_group infiniswap-remote-only || fail "Remote-Only device did not stop"
new_logs=$(dmesg | tail -n "+$((dmesg_start + 1))")
if grep -Eqi 'BUG:|Oops:|kernel panic|KASAN:|use-after-free|general protection fault' \
  <<<"$new_logs"; then
  fail "kernel log contains a fatal diagnostic"
fi

echo "Remote-Only Mode test passed"
