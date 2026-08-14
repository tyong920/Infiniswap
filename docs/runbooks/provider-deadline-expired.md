# Provider Failure Deadline expired

## Preflight

1. Capture `infiniswapctl status <device> --json`, `infiniswapctl metrics <device>`, and recent kernel/provider journals.
2. Compare the oldest in-flight I/O with the configured Provider Failure Deadline.
3. Identify whether the device is Backed Mode or Remote-Only Mode before touching the Provider.

## Expected output

`provider_timeouts_total` increases once per expired operation. Backed Mode falls back to a valid Backing Store; Remote-Only Mode enters Remote-Lost when the failed Provider held data. Late completions may increase without completing a request twice.

## Abort conditions

- Abort attempts to raise the deadline above 30 seconds or mask repeat expiries.
- Stop if Backed Mode also reports Backing-Degraded.
- Do not declare Remote-Only recovery without recreating a Remote-Lost device.

## Recovery and rollback

Restore the Provider or RDMA path, then drain/recreate the Consumer device so the failed Provider rejoins. Confirm the connection alert clears. The event-rate alert clears five minutes after the last expiry. Recreate terminal devices only after workload recovery. If deadlines continue, drain the Consumer and return to the Local Swap Baseline; preserve status and logs for diagnosis.
