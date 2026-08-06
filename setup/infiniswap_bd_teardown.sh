#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID:-$(id -u)} -ne 0 ]]; then
  echo "run as root" >&2
  exit 2
fi

name=${DEVICE_NAME:-infiniswap0}
root=/sys/kernel/config/infiniswap
group=$root/$name

if [[ ! $name =~ ^[[:alnum:]][[:alnum:]_-]*$ || ${#name} -ge 32 ]]; then
  echo "DEVICE_NAME must be 1-31 alphanumeric, '-' or '_' characters" >&2
  exit 2
fi

if [[ ! -d $group ]]; then
  echo "device configuration does not exist: $group" >&2
  exit 1
fi

# stop refuses active users, so callers must first close the device or disable
# this specific device as swap themselves.
printf 'stop\n' > "$group/state"
rmdir "$group"

if ! find "$root" -mindepth 1 -maxdepth 1 -type d -print -quit | grep -q .; then
  modprobe -r infiniswap
fi
