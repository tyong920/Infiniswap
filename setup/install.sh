#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ( $1 != "bd" && $1 != "daemon" ) ]]; then
  echo "Usage: $0 {bd|daemon}" >&2
  exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

max_page_num=${MAX_PAGE_NUM:-1}
bio_page_cap=${BIO_PAGE_CAP:-32}
max_remote_memory=${MAX_REMOTE_MEMORY_GB:-32}
stackbd_size=${DEVICE_SIZE_GB:-12}
stackbd_name=${DEVICE_NAME:-stackbd}
backing_store=${BACKING_STORE:-/dev/sda4}
provider_sample_size=${PROVIDER_SAMPLE_SIZE:-1}

max_client=${MAX_CLIENT:-32}
remote_memory_evict=${REMOTE_MEMORY_EVICT_GB:-8}
evict_hit_limit=${EVICT_HIT_LIMIT:-1}
remote_memory_expand=${REMOTE_MEMORY_EXPAND_GB:-16}
expand_hit_limit=${EXPAND_HIT_LIMIT:-20}
measured_free_mem_weight=${MEASURED_FREE_MEM_WEIGHT:-0.7}

if [[ $1 == "bd" ]]; then
  kdir=${KDIR:-/lib/modules/$(uname -r)/build}
  module_args=(
    "KDIR=$kdir"
    "INFINISWAP_MAX_PAGES_PER_REQUEST=$max_page_num"
    "INFINISWAP_BIO_PAGE_CAP=$bio_page_cap"
    "INFINISWAP_MAX_REMOTE_MEMORY_GB=$max_remote_memory"
    "INFINISWAP_DEVICE_SIZE_GB=$stackbd_size"
    "INFINISWAP_DEVICE_NAME=$stackbd_name"
    "INFINISWAP_BACKING_STORE=$backing_store"
    "INFINISWAP_PROVIDER_SAMPLE_SIZE=$provider_sample_size"
  )

  make -C "$repo_root/infiniswap_bd" "${module_args[@]}" modules
  sudo make -C "$repo_root/infiniswap_bd" "${module_args[@]}" install
  sudo install -D -m 0755 "$repo_root/infiniswap_bd/nbdxadm/nbdxadm" \
    /usr/local/bin/nbdxadm
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
