# Rollback to Local Swap Baseline

## Preflight

- Confirm the Local Swap Baseline device/file is intact, offline from Infiniswap, and has sufficient capacity.
- Stop new workload admission and capture status, metrics, audit log, kernel journal, and workload evidence.
- Verify enough RAM/alternate swap exists to disable and drain Infiniswap.

## Expected output

Only the named Infiniswap swap is disabled; its in-flight I/O drains; the Local Swap Baseline is enabled at the approved priority; verified workload I/O succeeds; Infiniswap Providers can then be stopped independently.

## Abort conditions

- Abort if the baseline has no valid swap signature, insufficient capacity, or unexpected dependencies.
- Stop on data mismatch, kernel oops/panic, or failed drain and escalate to controlled host recovery.
- Never use `swapoff -a` or modify unrelated swap.

## Recovery and rollback

Dry-run and then disable/drain the Infiniswap Device. Enable the validated Local Swap Baseline explicitly, verify `/proc/swaps`, workload health, and memory pressure, then stop/destroy Infiniswap when safe. Record `infiniswapctl audit cutover --subject <host> --change-id <id> --outcome rolled-back`. Re-enter Infiniswap only through the full normal-start preflight and release gates.
