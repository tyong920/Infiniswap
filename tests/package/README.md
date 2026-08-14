# Infiniswap package lifecycle test

`tests/package/run` builds the Debian package set and exercises install,
upgrade, downgrade, purge, and reinstall on the current disposable Ubuntu
host. It also verifies that package actions leave `/proc/swaps` unchanged,
preserve Provider conffiles, keep the Provider service disabled, include the
DKMS source/signing hooks, and build the packaged module against the selected
GA header tree. It writes checksum-ready DKMS evidence under `results/package/`
by default.

Run only on a disposable Ubuntu 22.04 or 24.04 test host:

```bash
sudo env \
  INFINISWAP_PACKAGE_TEST_DESTRUCTIVE=yes \
  INFINISWAP_PACKAGE_TEST_PREVIOUS_REF=<previous-release-tag> \
  tests/package/run
```

For the first packaged release only, omit `INFINISWAP_PACKAGE_TEST_PREVIOUS_REF`
to exercise a synthetic `0.0.9` bootstrap package. Later release gates supply
the actual previous packaged tag so maintainer scripts, configuration, schemas,
and DKMS source are tested across real release contents.

CI runs the package contract and binary-package build on every change for both
Ubuntu releases. The manual `package-lifecycle` workflow runs this destructive
harness only on explicitly labelled disposable Ubuntu 22.04 and 24.04 runners.
Physical canary evidence is deliberately external and is admitted only through
`infiniswapctl release-gate`.
