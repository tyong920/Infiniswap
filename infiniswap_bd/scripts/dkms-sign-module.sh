#!/usr/bin/env bash
set -euo pipefail

config=/etc/infiniswap/module-signing.conf
[[ -r $config ]] || exit 0
if [[ -L $config || ! -f $config || $(stat -c '%u' "$config") != 0 ]]; then
  echo "infiniswap-dkms: signing configuration must be a root-owned regular non-symlink file" >&2
  exit 2
fi
config_mode=$(stat -c '%a' "$config")
if (( (8#$config_mode & 022) != 0 )); then
  echo "infiniswap-dkms: signing configuration must not be group/other writable" >&2
  exit 2
fi
# Operators explicitly opt in by setting both paths. shellcheck disable=SC1091
source "$config"
key=${INFINISWAP_SIGN_KEY:-}
certificate=${INFINISWAP_SIGN_CERT:-}
if [[ -z $key && -z $certificate ]]; then
  exit 0
fi
if [[ -z $key || -z $certificate ]]; then
  echo "infiniswap-dkms: set both INFINISWAP_SIGN_KEY and INFINISWAP_SIGN_CERT" >&2
  exit 2
fi
if [[ -L $key || ! -f $key || $(stat -c '%u' "$key") != 0 ]]; then
  echo "infiniswap-dkms: signing key must be a root-owned regular non-symlink file" >&2
  exit 2
fi
key_mode=$(stat -c '%a' "$key")
if (( (8#$key_mode & 077) != 0 )); then
  echo "infiniswap-dkms: signing key must not be accessible by group or other" >&2
  exit 2
fi
if [[ ! -f $certificate ]]; then
  echo "infiniswap-dkms: signing certificate is not readable: $certificate" >&2
  exit 2
fi

kernel_release=${kernelver:-${1:-}}
kernel_headers=${kernel_source_dir:-/lib/modules/$kernel_release/build}
sign_file=$kernel_headers/scripts/sign-file
module=infiniswap_bd/infiniswap.ko
if [[ ! -x $sign_file || ! -f $module ]]; then
  echo "infiniswap-dkms: cannot sign $module with $sign_file" >&2
  exit 2
fi
"$sign_file" sha256 "$key" "$certificate" "$module"
echo "infiniswap-dkms: signed $module for $kernel_release"
