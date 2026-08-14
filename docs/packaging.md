# Infiniswap Debian packages and release operations

The source builds three version-locked Debian packages:

- `infiniswap-dkms`: Memory Consumer DKMS source and optional module-signing hook.
- `infiniswap-provider`: Memory Provider daemon, passive systemd unit, alert rules, and runbooks.
- `infiniswapctl`: administration application, schemas, migrations, upgrade preflight, and rollback commands.

Build binary packages on Ubuntu 22.04 or 24.04:

```bash
sudo apt-get install -y build-essential devscripts debhelper dkms \
  cmake pkg-config libibverbs-dev librdmacm-dev libssl-dev

dpkg-buildpackage --build=binary --no-sign
```

The packages are written to the parent directory. Install a reviewed, matching set with `apt-get install ./infiniswap-*.deb`. Package maintainer scripts do not load the Consumer module, create an Infiniswap Device, format a block device, call `swapon`/`swapoff`, or enable/start the Provider service.

## Fresh install

1. Install the matching package set and inspect `dkms status infiniswap`.
2. Copy Provider and Consumer examples from `/usr/share/infiniswap` into `/etc/infiniswap`; never edit the packaged examples in place.
3. Create root-owned mode-0600 PSK and allowlist files.
4. Validate JSON contracts with `infiniswapctl validate`.
5. Configure `/etc/default/infiniswap-provider`, then explicitly enable/start `infiniswap-provider.service` only after both `/etc/infiniswap/provider-memory.conf` and `/etc/infiniswap/consumers.conf` exist.
6. Create, format, and enable each Consumer through the separate reviewed `infiniswapctl` commands.

`infiniswap-provider.service` has `LimitMEMLOCK=infinity`, a loopback observability endpoint, and `ExecReload` for current/next PSK rotation or emergency revocation. The unit is deliberately disabled and not started by package installation.

## DKMS and module signing

DKMS accepts only the supported Ubuntu GA kernel lines, Linux 5.15 and 6.8. A missing exact header tree or unsupported ABI produces a direct remediation message in the DKMS build log. MLNX_OFED builds continue to discover the exact installed DKMS/header/symbol tuple through `infiniswap_bd/scripts/rdma-build-flags.sh`.

Modern DKMS installations may sign modules through their framework key. To use a dedicated operator-managed key instead, set both paths in `/etc/infiniswap/module-signing.conf`:

```ini
INFINISWAP_SIGN_KEY=/root/module-signing/infiniswap.key
INFINISWAP_SIGN_CERT=/root/module-signing/infiniswap.crt
```

The private key must be a root-owned non-symlink regular file with no group or other permissions. Packages contain no private key or certificate and never generate production key material.

## Explicit schema migration

Package upgrades preserve `/etc` conffiles and never rewrite configuration. Migrate one previous contract to a new file, review the diff, validate it on the intended host, and only then replace the active file:

```bash
infiniswapctl migrate consumer \
  --config /etc/infiniswap/consumer.json \
  --output /etc/infiniswap/consumer.v3.json
infiniswapctl validate consumer \
  --config /etc/infiniswap/consumer.v3.json
```

`provider` and `provider-directory` are also accepted migration kinds. Migration refuses current or unknown schema versions and refuses to overwrite an existing output path.

## Consumer upgrade

Stop new workload admission first. Preflight computes the target Infiniswap swap usage and verifies that `MemAvailable`, less the explicit reserve, plus free alternate swap can absorb it:

```bash
sudo infiniswapctl upgrade preflight infiniswap0 \
  --config /etc/infiniswap/consumer.json \
  --reserve-mib 4096
```

After reviewing a dry run, prepare the Consumer. The command records owner-only state, disables only the named swap, drains and destroys only the named device, and unloads only `infiniswap`:

```bash
sudo infiniswapctl upgrade prepare infiniswap0 \
  --config /etc/infiniswap/consumer.json \
  --state-file /var/lib/infiniswap/upgrades/change-123.json \
  --reserve-mib 4096 --dry-run
sudo infiniswapctl upgrade prepare infiniswap0 \
  --config /etc/infiniswap/consumer.json \
  --state-file /var/lib/infiniswap/upgrades/change-123.json \
  --reserve-mib 4096 --yes
sudo apt-get install ./infiniswap-dkms_NEW_all.deb ./infiniswapctl_NEW_all.deb
sudo infiniswapctl upgrade restore infiniswap0 \
  --config /etc/infiniswap/consumer.json \
  --state-file /var/lib/infiniswap/upgrades/change-123.json
```

Restore recreates the configured device but does not format or enable swap. Verify status and data expectations, then use the normal explicit format/enable procedure. Remote-Only Mode always stops and recreates; it has no in-place upgrade or retained data guarantee.

## Provider-first rolling upgrade

Current and previous protocol minors interoperate. Upgrade one Backed Mode Provider at a time, passing an explicit previous package artifact for automatic daemon-package rollback if `/healthz` fails:

```bash
sudo infiniswapctl upgrade provider \
  --consumer-mode backed \
  --target-package ./infiniswap-provider_NEW_amd64.deb \
  --rollback-package ./infiniswap-provider_OLD_amd64.deb \
  --dry-run
sudo infiniswapctl upgrade provider \
  --consumer-mode backed \
  --target-package ./infiniswap-provider_NEW_amd64.deb \
  --rollback-package ./infiniswap-provider_OLD_amd64.deb \
  --yes
```

The command refuses Remote-Only Mode. After each Provider, drain/recreate the Backed Mode Consumer and require every configured Provider healthy before touching the next Provider.

## Rollback and downgrade

Keep the complete previous `.deb` set before upgrading. After `upgrade prepare`, restore the previous module, CLI/schema files, and device configuration with explicit artifacts:

```bash
sudo infiniswapctl upgrade rollback infiniswap0 \
  --config /etc/infiniswap/consumer.json \
  --state-file /var/lib/infiniswap/upgrades/change-123.json \
  --package ./infiniswap-dkms_OLD_all.deb \
  --package ./infiniswapctl_OLD_all.deb \
  --yes
```

The rollback command verifies the captured configuration digest, installs only the named artifacts with downgrade enabled, reloads the module through normal device creation, and leaves swap disabled. Roll a Provider back with `upgrade provider` by swapping the target/rollback artifacts. Purge removes packaged files but follows Debian conffile semantics; preserve `/etc/infiniswap` and release evidence independently when an operational rollback is still possible.

## Kernel ABI release gate

A kernel ABI remains pinned until DKMS compile, certifiable VM regression, and short physical canary reports all pass for the same commit. Each report is checksummed in a schema-version-1 evidence document:

```json
{
  "schema_version": 1,
  "commit": "0123456789abcdef0123456789abcdef01234567",
  "ubuntu_release": "24.04",
  "kernel_release": "6.8.0-51-generic",
  "rdma_stack": "inbox",
  "dkms": {"path": "evidence/dkms.json", "sha256": "...", "passed": true},
  "vm": {"path": "evidence/vm/report.json", "sha256": "...", "passed": true, "certifiable": true},
  "canary": {"path": "evidence/canary.json", "sha256": "...", "passed": true, "duration_seconds": 3600}
}
```

The lifecycle harness writes the DKMS report named by
`INFINISWAP_PACKAGE_TEST_REPORT`; the VM harness embeds `source_commit`, the
exact guest kernel ABI, inbox-RDMA stack, certifiable profile, matrix outcomes,
cleanup, and host safety in its `report.json`. A physical canary report is a
schema-version-1 JSON object with `kind` set to `infiniswap.canary-report`,
`status` set to `passed`, the same `source_commit`, `ubuntu_release`,
`kernel_release`, and `rdma_stack` as the gate, plus its positive
`duration_seconds`. Compute each report checksum only after the report is final.

Verify the combined evidence before package promotion:

```bash
infiniswapctl release-gate --evidence evidence/kernel-abi.json \
  --commit "$(git rev-parse HEAD)"
```

Production package promotion requires the manual `kernel-abi-gate` workflow on
the managed release-gate runner; it checks out the exact commit and accepts only
an evidence manifest below `/var/lib/infiniswap-release-evidence`. The package
lifecycle workflow should receive the actual previous packaged tag through its
`previous_ref` input after the bootstrap release.

The gate accepts only Ubuntu 22.04/Linux 5.15 with inbox RDMA or MLNX_OFED 5.8, and Ubuntu 24.04/Linux 6.8 with inbox RDMA.
