#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ( $1 != "bd" && $1 != "daemon" ) ]]; then
  echo "Usage: $0 {bd|daemon}" >&2
  exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

rdma_root=${INFINISWAP_RDMA_ROOT:-}

if [[ $1 == "bd" ]]; then
  kdir=${KDIR:-/lib/modules/$(uname -r)/build}
  module_args=("KDIR=$kdir")
  if [[ -n $rdma_root ]]; then
    module_args+=("INFINISWAP_RDMA_ROOT=$rdma_root")
  fi

  make -C "$repo_root/infiniswap_bd" "${module_args[@]}" modules
  sudo make -C "$repo_root/infiniswap_bd" "${module_args[@]}" install
else
  build_dir=${BUILD_DIR:-$repo_root/build/daemon}
  cmake_args=(
    -S "$repo_root/infiniswap_daemon"
    -B "$build_dir"
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
  )

  cmake "${cmake_args[@]}"
  cmake --build "$build_dir"
  ctest --test-dir "$build_dir" --output-on-failure
fi
