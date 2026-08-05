#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
discover="$script_dir/../scripts/rdma-build-flags.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

kernel_release=5.15.0-test-generic
version_root="$tmp/dkms/mlnx-ofed-kernel/5.8-test"
tuple="$version_root/$kernel_release/x86_64"
ofa_tuple="$tmp/ofa/x86_64/$kernel_release"
mkdir -p "$tuple" "$ofa_tuple/include/linux"
: > "$ofa_tuple/include/linux/compat-2.6.h"
: > "$ofa_tuple/Module.symvers"

detected=$(INFINISWAP_DKMS_ROOT="$tmp/dkms" \
  INFINISWAP_OFA_ROOT="$tmp/ofa" \
  "$discover" detected "$kernel_release")
cflags=$(INFINISWAP_DKMS_ROOT="$tmp/dkms" \
  INFINISWAP_OFA_ROOT="$tmp/ofa" \
  "$discover" cflags "$kernel_release")
symvers=$(INFINISWAP_DKMS_ROOT="$tmp/dkms" \
  INFINISWAP_OFA_ROOT="$tmp/ofa" \
  "$discover" symvers "$kernel_release")

expected_cflags="-I$ofa_tuple/include -include $ofa_tuple/include/linux/compat-2.6.h"
test "$detected" = yes
test "$cflags" = "$expected_cflags"
test "$symvers" = "$ofa_tuple/Module.symvers"

if "$discover" cflags "$kernel_release" "$tmp/missing" >/dev/null 2>&1; then
  echo "missing explicit RDMA roots must fail" >&2
  exit 1
fi
