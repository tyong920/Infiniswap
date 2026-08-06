#!/usr/bin/env bash
set -euo pipefail

if [[ -z ${CONSUMER_CONFIG:-} ]]; then
  echo "CONSUMER_CONFIG must name a versioned Consumer JSON configuration" >&2
  exit 2
fi

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)
exec "$repo_root/bin/infiniswapctl" create --config "$CONSUMER_CONFIG" "$@"
