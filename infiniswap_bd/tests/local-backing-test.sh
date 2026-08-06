#!/usr/bin/env bash
set -euo pipefail

# This test overwrites the beginning of a dedicated test block device. Run it
# only in a disposable VM with a spare virtio/SCSI/NVMe disk or partition.
if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "run as root" >&2
  exit 2
fi
if [[ ${INFINISWAP_TEST_DESTRUCTIVE:-} != yes ]]; then
  echo "set INFINISWAP_TEST_DESTRUCTIVE=yes to acknowledge destructive I/O" >&2
  exit 2
fi

backing=${INFINISWAP_TEST_BACKING:-}
module=${INFINISWAP_TEST_MODULE:-}
configfs=/sys/kernel/config
root=$configfs/infiniswap
capacity_bytes=$((64 * 1024 * 1024))
loaded_module=0
mounted_configfs=0
loop_device=
dm_error_name=
tmp=$(mktemp -d)
declare -a created_groups=()

fail() {
  echo "local Backing Store test failed: $*" >&2
  exit 1
}

write_must_fail() {
  local value=$1
  local attribute=$2

  if printf '%s\n' "$value" > "$attribute" 2>/dev/null; then
    fail "write unexpectedly succeeded: $attribute=$value"
  fi
}

wait_for_path() {
  local path=$1
  local expected=$2
  local attempt

  for ((attempt = 0; attempt < 100; attempt++)); do
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

create_group() {
  local name=$1

  mkdir "$root/$name"
  created_groups+=("$name")
}

configure_group() {
  local name=$1
  local group_backing=$2
  local group_capacity=$3
  local group=$root/$name

  create_group "$name"
  printf 'backed\n' > "$group/mode"
  printf '%s\n' "$group_backing" > "$group/backing_store"
  printf '%s\n' "$group_capacity" > "$group/capacity_bytes"
}

stop_and_remove() {
  local name=$1
  local group=$root/$name

  if [[ -d $group ]]; then
    if [[ -w $group/state ]]; then
      printf 'stop\n' > "$group/state" 2>/dev/null || true
    fi
    rmdir "$group" 2>/dev/null || true
  fi
}

cleanup() {
  local name
  local index

  set +e
  for ((index = ${#created_groups[@]} - 1; index >= 0; index--)); do
    name=${created_groups[index]}
    stop_and_remove "$name"
  done
  if [[ -n $dm_error_name ]]; then
    dmsetup remove "$dm_error_name"
  fi
  if [[ -n $loop_device ]]; then
    losetup -d "$loop_device"
  fi
  if (( loaded_module )); then
    modprobe -r infiniswap
  fi
  if (( mounted_configfs )); then
    umount "$configfs"
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

for command in awk blockdev dd dmesg dmsetup findmnt fio grep insmod losetup \
  lsblk modprobe mount mountpoint readlink tail timeout truncate umount; do
  command -v "$command" >/dev/null || fail "missing command: $command"
done
dmesg_start=$(dmesg | wc -l)
backing=$(readlink -f "$backing")
[[ -b $backing ]] || fail "INFINISWAP_TEST_BACKING must name a block device"
backing_type=$(lsblk -dnro TYPE "$backing")
case $backing_type in
  disk|part|lvm) ;;
  *) fail "the Backing Store must be a disk, partition, or LVM LV, not $backing_type" ;;
esac
if lsblk -snro TYPE "$backing" | grep -Fxq loop; then
  fail "the Backing Store contains a loop device"
fi

while read -r child child_devno; do
  if findmnt -rn -o MAJ:MIN | grep -Fxq "$child_devno"; then
    fail "$child is mounted"
  fi
  while read -r swap_path _; do
    if [[ -b $swap_path ]] &&
       [[ $(lsblk -dnro MAJ:MIN "$swap_path") == "$child_devno" ]]; then
      fail "$child is active swap"
    fi
  done < <(tail -n +2 /proc/swaps)
  holder_dir=/sys/class/block/${child##*/}/holders
  if [[ -d $holder_dir ]] && compgen -G "$holder_dir/*" >/dev/null; then
    fail "$child has an active block holder"
  fi
done < <(lsblk -nrpo NAME,MAJ:MIN "$backing")

backing_bytes=$(blockdev --getsize64 "$backing")
(( backing_bytes >= capacity_bytes )) || fail "$backing is smaller than $capacity_bytes bytes"

if [[ -d /sys/module/infiniswap ]]; then
  fail "infiniswap is already loaded; use a disposable VM dedicated to this test"
fi
if ! mountpoint -q "$configfs"; then
  mount -t configfs none "$configfs"
  mounted_configfs=1
fi
if [[ -n $module ]]; then
  insmod "$module"
else
  modprobe infiniswap
fi
loaded_module=1
[[ -d $root ]] || fail "configfs subsystem was not registered"

# Ordinary files and loop devices are rejected at the configuration seam.
create_group invalid-file
truncate -s "$capacity_bytes" "$tmp/backing.img"
write_must_fail "$tmp/backing.img" "$root/invalid-file/backing_store"
rmdir "$root/invalid-file"

loop_device=$(losetup --find --show "$tmp/backing.img")
create_group invalid-loop
write_must_fail "$loop_device" "$root/invalid-loop/backing_store"
rmdir "$root/invalid-loop"
losetup -d "$loop_device"
loop_device=

# An undersized Backing Store fails activation before a disk is exposed.
configure_group undersized "$backing" "$((backing_bytes + 512))"
write_must_fail activate "$root/undersized/state"
[[ $(<"$root/undersized/state") == created ]] || fail "failed activation changed lifecycle state"
[[ ! -e /dev/undersized ]] || fail "undersized device was exposed"
rmdir "$root/undersized"

# A device-mapper error target verifies accepted backing-I/O failures complete.
modprobe dm_mod
dm_error_name="infiniswap-error-$$"
printf '0 %s error\n' "$((capacity_bytes / 512))" | \
  dmsetup create "$dm_error_name"
configure_group backing-error "/dev/mapper/$dm_error_name" "$capacity_bytes"
printf 'activate\n' > "$root/backing-error/state"
wait_for_path /dev/backing-error present
set +e
timeout 10 dd if=/dev/zero of=/dev/backing-error bs=4096 count=1 \
  oflag=direct status=none
dd_status=$?
set -e
case $dd_status in
  0) fail "write to an error Backing Store unexpectedly succeeded" ;;
  124) fail "write to an error Backing Store did not complete" ;;
esac
printf 'stop\n' > "$root/backing-error/state"
wait_for_path /dev/backing-error absent
rmdir "$root/backing-error"
dmsetup remove "$dm_error_name"
dm_error_name=

# Concurrent activation has one owner and leaves one balanced dependency.
configure_group concurrent-activate "$backing" "$capacity_bytes"
set +e
(printf 'activate\n' > "$root/concurrent-activate/state" 2>/dev/null) &
activate_one=$!
(printf 'activate\n' > "$root/concurrent-activate/state" 2>/dev/null) &
activate_two=$!
wait "$activate_one"
activate_one_status=$?
wait "$activate_two"
activate_two_status=$?
set -e
(( activate_one_status + activate_two_status == 1 )) || \
  fail "concurrent activation did not produce exactly one owner"
[[ $(<"$root/concurrent-activate/state") == active ]] || \
  fail "concurrent activation did not leave an active device"
printf 'stop\n' > "$root/concurrent-activate/state"
rmdir "$root/concurrent-activate"

run_io_verification() {
  local name=$1
  local group=$root/$name
  local device=/dev/$name
  local logical_size

  configure_group "$name" "$backing" "$capacity_bytes"
  [[ $(<"$group/state") == created ]] || fail "$name was not created"
  [[ ! -e $device ]] || fail "$name was exposed before activation"

  printf 'activate\n' > "$group/state"
  [[ $(<"$group/state") == active ]] || fail "$name did not activate"
  wait_for_path "$device" present

  exec 9<>"$device"
  write_must_fail stop "$group/state"
  [[ $(<"$group/state") == active ]] || fail "busy stop changed lifecycle state"
  exec 9>&-

  [[ $(<"/sys/block/$name/queue/discard_max_bytes") == 0 ]] || fail "discard is advertised"
  [[ $(<"/sys/block/$name/queue/write_zeroes_max_bytes") == 0 ]] || fail "write zeroes is advertised"
  blockdev --flushbufs "$device"

  fio --name=mixed-q1 --filename="$device" --direct=1 --ioengine=libaio \
    --rw=randrw --rwmixread=60 --bs=4k --iodepth=1 --size=48m \
    --verify=crc32c --do_verify=1 --verify_fatal=1 --group_reporting
  fio --name=mixed-q32 --filename="$device" --direct=1 --ioengine=libaio \
    --rw=randrw --rwmixread=60 --bs=4k --iodepth=32 --size=48m \
    --verify=crc32c --do_verify=1 --verify_fatal=1 --group_reporting

  logical_size=$(blockdev --getss "$device")
  fio --name=boundary --filename="$device" --direct=1 --ioengine=libaio \
    --rw=write --offset="$((logical_size * 7))" --bs="$((logical_size * 3))" \
    --size="$((logical_size * 30))" --verify=crc32c --do_verify=1 \
    --verify_fatal=1 --group_reporting
  dd if=/dev/zero of="$device" bs="$logical_size" count=1 status=none

  printf 'drain\n' > "$group/state"
  [[ $(<"$group/state") == drained ]] || fail "$name did not drain"
  wait_for_path "$device" absent
  printf 'stop\n' > "$group/state"
  [[ $(<"$group/state") == stopped ]] || fail "$name did not stop"
  rmdir "$group"
}

run_io_verification infiniswap-test

# Repeated activation and destruction catches stale minors, queues, and holders.
for iteration in {1..10}; do
  name="is-repeat-$iteration"
  group="$root/$name"
  configure_group "$name" "$backing" "$capacity_bytes"
  printf 'activate\n' > "$group/state"
  wait_for_path "/dev/$name" present
  printf 'stop\n' > "$group/state"
  wait_for_path "/dev/$name" absent
  rmdir "$group"
done

# With no devices, unload/reload must leave configfs and the major reusable.
modprobe -r infiniswap
loaded_module=0
if [[ -n $module ]]; then
  insmod "$module"
else
  modprobe infiniswap
fi
loaded_module=1
[[ -d $root ]] || fail "configfs subsystem was not restored after reload"

dmesg | tail -n "+$((dmesg_start + 1))" > "$tmp/kernel.log"
if grep -Eiq 'BUG:|WARNING:|Oops:|kernel panic|use-after-free|refcount.*underflow' \
    "$tmp/kernel.log"; then
  cat "$tmp/kernel.log" >&2
  fail "kernel diagnostics reported a correctness failure"
fi

echo "local Backing Store lifecycle and I/O verification passed"
