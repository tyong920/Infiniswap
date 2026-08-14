# Consumer drain

## Preflight

- Estimate available RAM and alternate swap needed to absorb current Infiniswap usage.
- Stop new workload admission and run `infiniswapctl status <device>`.
- Confirm the named device path and that unrelated swap remains online.

## Expected output

`swapoff` removes only the Infiniswap Device, lifecycle reaches `drained`, in-flight I/O reaches zero, and other `/proc/swaps` entries are unchanged.

## Abort conditions

- Abort before `swapoff` if available RAM/alternate swap is insufficient.
- Abort destruction if swap remains enabled or lifecycle is not drained/stopped.
- Do not force module unload while openers or I/O remain.

## Recovery and rollback

Run disable, then drain, inspect zero in-flight I/O, and destroy only when an upgrade/recreation requires it. To abort before destroy, correct capacity and re-enable at the configured priority. After destroy, recreate from validated configuration. If recreation fails, leave Infiniswap disabled and retain the Local Swap Baseline.
