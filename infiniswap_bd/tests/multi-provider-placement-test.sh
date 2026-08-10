#!/usr/bin/env bash
set -euo pipefail

# Destructive Soft-RoCE certification for Power-of-d multi-Provider placement.
# Starts two (or three) Provider daemons on one Soft-RoCE host and one Consumer.
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
provider_count=${INFINISWAP_TEST_PROVIDER_COUNT:-2}
base_port=${INFINISWAP_TEST_PROVIDER_PORT:-19420}
failure_deadline_ms=${INFINISWAP_TEST_FAILURE_DEADLINE_MS:-2000}
capacity_bytes=$((2 * 1024 * 1024 * 1024))
chunk_bytes=$((1024 * 1024 * 1024))
configfs=/sys/kernel/config
root=$configfs/infiniswap
tmp=$(mktemp -d)
loaded_module=0
mounted_configfs=0
declare -a created_groups=()
declare -a provider_pids=()
declare -a provider_ports=()
declare -a provider_psks=()

fail() {
  echo "multi-Provider placement test failed: $*" >&2
  local index
  for ((index = 0; index < ${#provider_pids[@]}; index++)); do
    [[ -f $tmp/provider-$index.log ]] && {
      echo "--- provider $index log ---" >&2
      tail -80 "$tmp/provider-$index.log" >&2
    }
  done
  exit 1
}

wait_for_value() {
  local path=$1
  local expected=$2
  local attempt
  local actual

  for ((attempt = 0; attempt < 300; attempt++)); do
    actual=$(cat "$path" 2>/dev/null || echo missing)
    if [[ $actual == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  fail "$path did not become $expected (got '$(cat "$path" 2>/dev/null || echo missing)')"
}

wait_for_connection() {
  local group=$1
  local attempt
  local state

  for ((attempt = 0; attempt < 300; attempt++)); do
    state=$(cat "$group/connection_state" 2>/dev/null || echo missing)
    if [[ $state == connected || $state == degraded ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "--- dmesg ---" >&2
  dmesg | tail -40 >&2
  fail "$group/connection_state did not become connected/degraded (got '$state' exclusions='$(cat "$group/provider_exclusions" 2>/dev/null || true)' last_error='$(cat "$group/last_error" 2>/dev/null || true)')"
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
  [[ $(cat "$group/state") == stopped ]] || return 1
  wait_for_path "/dev/$name" absent
  rmdir "$group"
}

recycle_soft_roce() {
  local netdev=

  [[ $rail == rxe* ]] || return 0
  for netdev in ens3 eth0 enp0s3; do
    [[ -d /sys/class/net/$netdev ]] && break
  done
  rdma link delete "$rail" 2>/dev/null || true
  sleep 0.3
  # Soft-RoCE keeps wedged QP state across link delete/add; reload the
  # software RoCE module so a recreate gets a clean transport.
  if [[ -d /sys/module/rdma_rxe ]]; then
    modprobe -r rdma_rxe || fail "could not unload rdma_rxe before Soft-RoCE recycle"
  fi
  sleep 0.3
  modprobe rdma_rxe || fail "could not load rdma_rxe"
  sleep 0.3
  rdma link add "$rail" type rxe netdev "$netdev" || \
    fail "could not recreate Soft-RoCE link $rail on $netdev"
  rdma link show "$rail/1" >/dev/null || fail "Soft-RoCE link $rail missing after recreate"
}

cleanup() {
  local index

  set +e
  for ((index = ${#created_groups[@]} - 1; index >= 0; index--)); do
    stop_group "${created_groups[index]}"
  done
  for pid in "${provider_pids[@]:-}"; do
    [[ -n $pid ]] || continue
    kill -TERM "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
  done
  if ((loaded_module)); then
    rmmod infiniswap
  fi
  if ((mounted_configfs)); then
    umount "$configfs"
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

((provider_count == 2 || provider_count == 3)) || \
  fail "INFINISWAP_TEST_PROVIDER_COUNT must be 2 or 3"
for command in cmp dd dmesg insmod kill openssl rdma rmmod; do
  command -v "$command" >/dev/null || fail "missing command: $command"
done
[[ -n $backing && -n $address && -n $provider && -n $module ]] || \
  fail "set BACKING, RDMA_ADDRESS, PROVIDER, and MODULE"
[[ -x $provider ]] || fail "Provider executable is not executable: $provider"
[[ -f $module ]] || fail "module does not exist: $module"
rdma link show "$rail/1" >/dev/null 2>&1 || fail "RDMA Rail unavailable: $rail"
recycle_soft_roce
backing=$(readlink -f "$backing")
[[ -b $backing ]] || fail "Backing Store must be a block device"
(( $(blockdev --getsize64 "$backing") >= capacity_bytes )) || \
  fail "Backing Store must provide at least 2 GiB"
numa_path="/sys/class/infiniband/$rail/device/numa_node"
if [[ -r $numa_path ]]; then
  numa_node=$(cat "$numa_path")
else
  numa_node=-1
fi
[[ -d /sys/module/infiniswap ]] && \
  fail "infiniswap is already loaded; use a dedicated disposable VM"

ulimit -l unlimited 2>/dev/null || fail "could not raise memlock"
for ((index = 0; index < provider_count; index++)); do
  port=$((base_port + index))
  psk=$(openssl rand -hex 32)
  provider_ports+=("$port")
  provider_psks+=("$psk")
  cat > "$tmp/provider-$index-memory.conf" <<EOF
version = 1
host_reserve_gib = 0
max_opportunistic_gib = 1
max_committed_gib = 0
EOF
  cat > "$tmp/provider-$index-consumers.conf" <<EOF
version = 1

[consumer:consumer-test]
current_key_id = key-test
current_psk_hex = $psk
max_connections = 1
max_opportunistic_gib = 1
max_committed_gib = 0
revoked = false
EOF
  chmod 0600 "$tmp/provider-$index-consumers.conf"
  stdbuf -oL -eL "$provider" :: "$port" "$tmp/provider-$index-memory.conf" \
    "$tmp/provider-$index-consumers.conf" >"$tmp/provider-$index.log" 2>&1 &
  provider_pids+=("$!")
  sleep 0.5
  kill -0 "${provider_pids[index]}" 2>/dev/null || \
    fail "Provider $index did not start on port $port"
done

if ! mountpoint -q "$configfs"; then
  mount -t configfs none "$configfs"
  mounted_configfs=1
fi
insmod "$module"
loaded_module=1
[[ -d $root ]] || fail "configfs subsystem was not registered"

name=infiniswap-multi
group=$root/$name
mkdir "$group"
created_groups+=("$name")
printf 'backed\n' > "$group/mode"
printf 'strict\n' > "$group/acknowledgement_policy"
printf '%s\n' "$backing" > "$group/backing_store"
printf '%s\n' "$capacity_bytes" > "$group/capacity_bytes"
printf '%s\n' "$failure_deadline_ms" > "$group/provider_failure_deadline_ms"
# High enough that incidental udev/fsync reads do not promote every chunk.
printf '64\n' > "$group/hot_range_threshold"
printf '1\n' > "$group/hot_range_read_weight"
printf '1\n' > "$group/hot_range_write_weight"
printf 'consumer-test\n' > "$group/consumer_id"
provider_list=""
for ((index = 0; index < provider_count; index++)); do
  if [[ -n $provider_list ]]; then
    provider_list+=","
  fi
  provider_list+="provider-$index"
done
printf '%s\n' "$provider_list" > "$group/providers"
printf '%s\n' "$provider_count" > "$group/placement_sample_size"
printf '7\n' > "$group/placement_seed"
for ((index = 0; index < provider_count; index++)); do
  weight=$((100 + index * 50))
  printf 'provider-%s\n' "$index" > "$group/provider_bind"
  printf '%s\n' "$address" > "$group/provider_address"
  printf '%s\n' "${provider_ports[index]}" > "$group/provider_port"
  printf '%s\n' "$rail" > "$group/rdma_device"
  printf '1\n' > "$group/rdma_port"
  printf '%s\n' "$numa_node" > "$group/rdma_numa_node"
  printf 'key-test\n' > "$group/provider_key_id"
  printf '%s\n' "${provider_psks[index]}" > "$group/provider_psk"
  printf '%s\n' "$weight" > "$group/placement_weight"
done
printf '100\n' > "$group/swap_priority"
printf 'activate\n' > "$group/state"
wait_for_path "/dev/$name" present
wait_for_connection "$group"
# Let device discovery I/O settle before intentional Hot Range traffic.
sleep 1

# Heat two distinct 1 GiB ranges so Power-of-d places two Remote Chunks.
dd if=/dev/urandom of="$tmp/seed0" bs=4096 count=64 status=none
dd if="$tmp/seed0" of="/dev/$name" bs=4096 count=64 oflag=direct conv=fsync status=none
for ((attempt = 0; attempt < 300; attempt++)); do
  placements=$(cat "$group/remote_chunk_placements")
  mapped=$(cat "$group/mapped_remote_chunks")
  [[ $placements == 0:provider-* && $mapped -ge 1 ]] && break
  sleep 0.1
done
[[ $(cat "$group/mapped_remote_chunks") -ge 1 ]] || \
  fail "first Hot Range was not placed: $(cat "$group/remote_chunk_placements")"

dd if=/dev/urandom of="$tmp/seed1" bs=4096 count=64 status=none
dd if="$tmp/seed1" of="/dev/$name" bs=4096 count=64 seek=$((chunk_bytes / 4096)) \
  oflag=direct conv=fsync status=none
for ((attempt = 0; attempt < 300; attempt++)); do
  mapped=$(cat "$group/mapped_remote_chunks")
  ((mapped >= 2)) && break
  sleep 0.1
done
placements=$(cat "$group/remote_chunk_placements")
(( $(cat "$group/mapped_remote_chunks") >= 2 )) || \
  fail "expected two mapped Remote Chunks, got $(cat "$group/mapped_remote_chunks") placements=$placements"
echo "placements after two Hot Ranges: $placements"
# With equal Provider capacity, Power-of-d must prefer remaining capacity after
# the first map, so the second Remote Chunk lands on a different Provider.
[[ $placements == *" "* ]] || \
  fail "expected two placement records, got '$placements'"
first_provider=${placements%% *}
first_provider=${first_provider#*:}
second_provider=${placements##* }
second_provider=${second_provider#*:}
[[ $first_provider != "$second_provider" ]] || \
  fail "expected capacity-driven spread across Providers, got '$placements'"

# Deterministic seed must be stable across recreate for the same Provider set.
first_placements=$placements
stop_group "$name" || fail "first multi-Provider device did not tear down"
# Soft-RoCE loopback wedges after multi-QP teardown. Recycle the rail only
# after the Consumer module releases its RDMA resources; then reload.
for pid in "${provider_pids[@]:-}"; do
  [[ -n $pid ]] || continue
  kill -TERM "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
done
provider_pids=()
if ((loaded_module)); then
  rmmod infiniswap || fail "could not unload infiniswap before Soft-RoCE recycle"
  loaded_module=0
fi
recycle_soft_roce
sleep 2
for ((index = 0; index < provider_count; index++)); do
  stdbuf -oL -eL "$provider" :: "${provider_ports[index]}" \
    "$tmp/provider-$index-memory.conf" \
    "$tmp/provider-$index-consumers.conf" >"$tmp/provider-$index.log" 2>&1 &
  provider_pids+=("$!")
  sleep 0.5
  kill -0 "${provider_pids[index]}" 2>/dev/null || \
    fail "Provider $index did not restart on port ${provider_ports[index]}"
done
insmod "$module" || fail "could not reload infiniswap after Soft-RoCE recycle"
loaded_module=1
created_groups=()
mkdir "$group"
created_groups+=("$name")
printf 'backed\n' > "$group/mode"
printf 'strict\n' > "$group/acknowledgement_policy"
printf '%s\n' "$backing" > "$group/backing_store"
printf '%s\n' "$capacity_bytes" > "$group/capacity_bytes"
printf '%s\n' "$failure_deadline_ms" > "$group/provider_failure_deadline_ms"
# High enough that incidental udev/fsync reads do not promote every chunk.
printf '64\n' > "$group/hot_range_threshold"
printf '1\n' > "$group/hot_range_read_weight"
printf '1\n' > "$group/hot_range_write_weight"
printf 'consumer-test\n' > "$group/consumer_id"
printf '%s\n' "$provider_list" > "$group/providers"
printf '%s\n' "$provider_count" > "$group/placement_sample_size"
printf '7\n' > "$group/placement_seed"
for ((index = 0; index < provider_count; index++)); do
  weight=$((100 + index * 50))
  printf 'provider-%s\n' "$index" > "$group/provider_bind"
  printf '%s\n' "$address" > "$group/provider_address"
  printf '%s\n' "${provider_ports[index]}" > "$group/provider_port"
  printf '%s\n' "$rail" > "$group/rdma_device"
  printf '1\n' > "$group/rdma_port"
  printf '%s\n' "$numa_node" > "$group/rdma_numa_node"
  printf 'key-test\n' > "$group/provider_key_id"
  printf '%s\n' "${provider_psks[index]}" > "$group/provider_psk"
  printf '%s\n' "$weight" > "$group/placement_weight"
done
printf '100\n' > "$group/swap_priority"
printf 'activate\n' > "$group/state"
wait_for_path "/dev/$name" present
wait_for_connection "$group"
sleep 1
dd if="$tmp/seed0" of="/dev/$name" bs=4096 count=64 oflag=direct conv=fsync status=none
for ((attempt = 0; attempt < 300; attempt++)); do
  (( $(cat "$group/mapped_remote_chunks") >= 1 )) && break
  sleep 0.1
done
dd if="$tmp/seed1" of="/dev/$name" bs=4096 count=64 seek=$((chunk_bytes / 4096)) \
  oflag=direct conv=fsync status=none
for ((attempt = 0; attempt < 300; attempt++)); do
  (( $(cat "$group/mapped_remote_chunks") >= 2 )) && break
  sleep 0.1
done
second_placements=$(cat "$group/remote_chunk_placements")
[[ $second_placements == "$first_placements" ]] || \
  fail "deterministic seed changed placements from '$first_placements' to '$second_placements'"

# Kill one Provider; Backed Mode must continue locally and record exclusion.
kill -KILL "${provider_pids[0]}"
wait "${provider_pids[0]}" 2>/dev/null || true
provider_pids[0]=
for ((attempt = 0; attempt < 400; attempt++)); do
  exclusions=$(cat "$group/provider_exclusions")
  [[ $exclusions == *provider-0* ]] && break
  state=$(cat "$group/connection_state")
  [[ $state == degraded || $state == connected ]] || true
  sleep 0.05
done
exclusions=$(cat "$group/provider_exclusions")
[[ $exclusions == *provider-0* ]] || \
  fail "Provider loss was not recorded in exclusions: $exclusions"
dd if=/dev/urandom of="$tmp/local-pattern" bs=4096 count=16 status=none
dd if="$tmp/local-pattern" of="/dev/$name" bs=4096 count=16 \
  seek=$((1536 * 1024 / 4)) oflag=direct conv=fsync status=none
dd if="/dev/$name" of="$tmp/local-actual" bs=4096 count=16 \
  skip=$((1536 * 1024 / 4)) iflag=direct status=none
cmp "$tmp/local-pattern" "$tmp/local-actual" || \
  fail "Backed Mode did not continue locally after Provider loss"

stop_group "$name" || fail "multi-Provider device did not tear down"
echo "Power-of-d ${provider_count}-Provider Soft-RoCE verification passed"
