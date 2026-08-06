#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "run as root" >&2
  exit 2
fi

name=${DEVICE_NAME:-infiniswap0}
backing_store=${BACKING_STORE:-}
capacity_bytes=${CAPACITY_BYTES:-}
configfs=/sys/kernel/config
group=$configfs/infiniswap/$name

if [[ ! $name =~ ^[[:alnum:]][[:alnum:]_-]*$ || ${#name} -ge 32 ]]; then
  echo "DEVICE_NAME must be 1-31 alphanumeric, '-' or '_' characters" >&2
  exit 2
fi

if [[ -z $backing_store || -z $capacity_bytes ]]; then
  echo "BACKING_STORE and CAPACITY_BYTES are required" >&2
  exit 2
fi
backing_store=$(readlink -f "$backing_store")
if [[ ! -b $backing_store ]]; then
  echo "BACKING_STORE must name a block device" >&2
  exit 2
fi
if lsblk -snro TYPE "$backing_store" | grep -Fxq loop; then
  echo "BACKING_STORE must not contain a loop device" >&2
  exit 2
fi

modprobe configfs
if ! mountpoint -q "$configfs"; then
  mount -t configfs none "$configfs"
fi
modprobe infiniswap

mkdir "$group"
cleanup_on_error=1
cleanup() {
  if (( cleanup_on_error )); then
    rmdir "$group" 2>/dev/null || true
  fi
}
trap cleanup EXIT

printf 'backed\n' > "$group/mode"
printf '%s\n' "$backing_store" > "$group/backing_store"
printf '%s\n' "$capacity_bytes" > "$group/capacity_bytes"
cleanup_on_error=0

echo "created inactive device configuration $group"
echo "activate explicitly with: echo activate > $group/state"
