#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ( $1 != "bd" && $1 != "daemon" ) ]]; then
  echo "Usage: $0 {bd|daemon}" >&2
  exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

max_remote_memory=${MAX_REMOTE_MEMORY_GB:-32}
rdma_root=${INFINISWAP_RDMA_ROOT:-}

max_client=${MAX_CLIENT:-32}
remote_memory_evict=${REMOTE_MEMORY_EVICT_GB:-8}
evict_hit_limit=${EVICT_HIT_LIMIT:-1}
remote_memory_expand=${REMOTE_MEMORY_EXPAND_GB:-16}
expand_hit_limit=${EXPAND_HIT_LIMIT:-20}
measured_free_mem_weight=${MEASURED_FREE_MEM_WEIGHT:-0.7}

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
    "-DINFINISWAP_MAX_CLIENT=$max_client"
    "-DINFINISWAP_MAX_REMOTE_MEMORY_GB=$max_remote_memory"
    "-DINFINISWAP_REMOTE_MEMORY_EVICT_GB=$remote_memory_evict"
    "-DINFINISWAP_EVICT_HIT_LIMIT=$evict_hit_limit"
    "-DINFINISWAP_REMOTE_MEMORY_EXPAND_GB=$remote_memory_expand"
    "-DINFINISWAP_EXPAND_HIT_LIMIT=$expand_hit_limit"
    "-DINFINISWAP_MEASURED_FREE_MEM_WEIGHT=$measured_free_mem_weight"
  )

  cmake "${cmake_args[@]}"
  cmake --build "$build_dir"
  ctest --test-dir "$build_dir" --output-on-failure
fi
