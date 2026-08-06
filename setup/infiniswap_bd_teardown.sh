#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || ( $1 != disable && $1 != drain && $1 != destroy ) ]]; then
  echo "Usage: $0 {disable|drain|destroy} [--dry-run]" >&2
  exit 2
fi

action=$1
shift
name=${DEVICE_NAME:-infiniswap0}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)
exec "$repo_root/bin/infiniswapctl" "$action" "$name" "$@"
