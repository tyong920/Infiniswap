# Backing-Degraded

## Preflight

1. Run `infiniswapctl status <device>` and inspect the Backing Store with `lsblk` and storage health tooling.
2. Verify alternate swap/RAM can absorb the Consumer drain.
3. Capture backing failure, retry, invalid-sector, Local-Only Data, rejected-write, and I/O counters.

## Expected output

The device reports `backing-degraded`; new writes are rejected and `backing_degraded_transitions_total` is nonzero. This state does not self-clear when the block device becomes reachable.

## Abort conditions

- Do not re-enable writes on the existing device.
- Abort drain if alternate memory capacity is insufficient or unrelated swap would be modified.
- Stop immediately on data mismatch, kernel oops, or failed in-flight-I/O drain.

## Recovery and rollback

Disable swap, drain the device, stop it, repair or replace the Backing Store, and recreate the device from validated configuration. Confirm healthy status before formatting/enabling. If recreation fails, leave Infiniswap disabled and use the Local Swap Baseline described in `rollback.md`.
