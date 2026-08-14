#!/usr/bin/env bash
set -euo pipefail

kernel_release=${kernelver:-${1:-}}
kernel_headers=${kernel_source_dir:-}

if [[ -z $kernel_release ]]; then
  echo "infiniswap-dkms: DKMS did not provide the target kernel release" >&2
  exit 2
fi
case $kernel_release in
  5.15.*|6.8.*) ;;
  *)
    echo "infiniswap-dkms: unsupported kernel $kernel_release; validated Ubuntu GA lines are 5.15 and 6.8" >&2
    exit 2
    ;;
esac
if [[ -z $kernel_headers ]]; then
  kernel_headers=/lib/modules/$kernel_release/build
fi
if [[ ! -f $kernel_headers/Makefile ]]; then
  echo "infiniswap-dkms: missing headers for $kernel_release at $kernel_headers" >&2
  echo "infiniswap-dkms: install the exact linux-headers-$kernel_release package and retry dkms autoinstall" >&2
  exit 2
fi

echo "infiniswap-dkms: validated build target $kernel_release ($kernel_headers)"
