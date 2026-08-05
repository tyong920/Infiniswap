#!/bin/sh
set -eu

usage()
{
  echo "usage: $0 {detected|cflags|symvers} KERNEL_RELEASE [RDMA_ROOT]" >&2
  exit 2
}

[ "$#" -eq 2 ] || [ "$#" -eq 3 ] || usage
mode=$1
kernel_release=$2
explicit_root=${3:-}
dkms_root=${INFINISWAP_DKMS_ROOT:-/var/lib/dkms}
ofa_root=${INFINISWAP_OFA_ROOT:-/usr/src/ofa_kernel}

case "$mode" in
  detected|cflags|symvers) ;;
  *) usage ;;
esac

roots=
append_root()
{
  [ -d "$1" ] || return 0
  case "
$roots
" in
    *"
$1
"*) ;;
    *) roots="${roots}${roots:+
}$1" ;;
  esac
}

if [ -n "$explicit_root" ]; then
  if [ ! -d "$explicit_root" ]; then
    echo "RDMA build root does not exist: $explicit_root" >&2
    exit 1
  fi
  append_root "$explicit_root"
else
  for ofa_tuple in "$ofa_root"/*/"$kernel_release" \
                   "$ofa_root/$kernel_release"; do
    append_root "$ofa_tuple"
  done
  for tuple in "$dkms_root"/mlnx-ofed-kernel/*/"$kernel_release"/*; do
    [ -d "$tuple" ] || continue
    version_root=$(dirname "$(dirname "$tuple")")
    append_root "$tuple/build"
    append_root "$tuple"
    append_root "$version_root/build"
    append_root "$version_root/source"
  done
fi

old_ifs=$IFS
IFS='
'
if [ "$mode" = detected ]; then
  if [ -n "$roots" ]; then
    printf 'yes\n'
  fi
elif [ "$mode" = cflags ]; then
  for root in $roots; do
    include_dir=$root/include
    [ -d "$include_dir" ] || continue
    printf '%s' "-I$include_dir"
    if [ -f "$include_dir/linux/compat-2.6.h" ]; then
      printf ' %s' "-include $include_dir/linux/compat-2.6.h"
    fi
    printf '\n'
    break
  done
else
  for root in $roots; do
    for symvers in "$root/Module.symvers" "$root/module/Module.symvers"; do
      if [ -f "$symvers" ]; then
        printf '%s\n' "$symvers"
        IFS=$old_ifs
        exit 0
      fi
    done
  done
fi
IFS=$old_ifs
