# Consumer drain

## Preflight

- Stop new workload admission and run `infiniswapctl status <device>`.
- Run `infiniswapctl upgrade preflight <device> --config <consumer.json> --reserve-mib <reserve>`; it rejects the drain unless available RAM above the reserve plus free alternate swap can absorb current Infiniswap swap usage.
- Confirm the named device path and that unrelated swap remains online.

## Expected output

`swapoff` removes only the Infiniswap Device, lifecycle reaches `drained`, in-flight I/O reaches zero, and other `/proc/swaps` entries are unchanged.

## Abort conditions

- Abort before `swapoff` if available RAM/alternate swap is insufficient.
- Abort destruction if swap remains enabled or lifecycle is not drained/stopped.
- Do not force module unload while openers or I/O remain.

## Recovery and rollback

Run `infiniswapctl upgrade prepare` with an owner-only state file to disable,
drain, destroy, and unload only the named Consumer. After package installation,
`infiniswapctl upgrade restore` recreates the validated device configuration but
leaves swap disabled. To abort before destroy, correct capacity and re-enable at
the configured priority. If the new release fails, use `infiniswapctl upgrade
rollback` with the captured state and explicit previous `.deb` artifacts. If
recreation fails, leave Infiniswap disabled and retain the Local Swap Baseline.
