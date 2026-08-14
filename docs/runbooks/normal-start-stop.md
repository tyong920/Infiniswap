# Normal start and stop

## Preflight

- Validate Consumer, Provider, and Provider Directory configuration with `infiniswapctl validate`.
- Confirm every RDMA Rail, Backing Store, Remote-Only eligibility declaration, Provider Failure Deadline, and explicit swap priority.
- Confirm no unrelated swap command appears in the change plan.

## Expected output

Start produces healthy Provider `/healthz`, connected Provider status, an active Infiniswap Device, a swap signature, and the configured priority in `/proc/swaps`. Stop removes only the named device after swap is disabled and I/O drains.

## Abort conditions

- Abort start on validation, authentication, capacity, or health failure.
- Abort stop if alternate RAM/swap is insufficient or in-flight I/O does not reach zero.
- Never format an enabled or mounted device.

## Recovery and rollback

Start: validate, create, inspect status, format once, then enable. Stop: disable, drain, verify zero in-flight I/O, then destroy. Every mutating command supports `--dry-run`. If any start gate fails, destroy the disabled device and retain the Local Swap Baseline. If stop cannot drain, stop workloads rather than forcing module removal.
