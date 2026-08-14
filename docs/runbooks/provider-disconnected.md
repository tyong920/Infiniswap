# Provider disconnected

## Preflight

1. Run `infiniswapctl status <device>` and `infiniswapctl alerts <device>`.
2. Confirm the named Memory Provider is expected in the Provider Directory.
3. Record the device mode, mapped Remote Chunks, and Provider Failure Deadline.
4. Check Provider process, RDMA Rail, routing, and host health without changing swap.

## Expected output

Healthy recovery shows every Provider as `connected`, the aggregate connection as `connected`, and no `InfiniswapProviderDisconnected` alert. Backed Mode continues from the Backing Store during the interruption. A Remote-Only Mode device may instead show `remote-lost`.

## Abort conditions

- Stop recovery changes if the host reports kernel oops/panic, data mismatch, or unrelated storage failure.
- Do not restart a Remote-Only Mode Provider in place and assume the device recovered after `remote-lost`.
- Do not disable unrelated swap or unload the module while the device has in-flight I/O.

## Recovery and rollback

Restore the Provider daemon or RDMA Rail. The initial implementation does not re-admit a failed Provider into an existing fabric, so drain and recreate the Consumer device to reconnect it; the alert clears after the recreated device reports every Provider connected. If the device is Remote-Lost, first stop affected workloads and follow `remote-lost.md`. If recreation fails, keep Backed Mode on its Backing Store or restore the Local Swap Baseline using `rollback.md`.
