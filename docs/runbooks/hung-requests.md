# Hung requests

## Preflight

1. Capture `inflight_io`, oldest in-flight age, completed-I/O counter, and Provider Failure Deadline twice one minute apart.
2. Check Provider/RDMA and Backing Store health without unloading the module.
3. Confirm whether the VM fault harness reproduces the stall.

## Expected output

The alert fires when I/O remains in flight beyond twice the configured deadline for one minute. Normal failure handling completes or falls back by the deadline, and the oldest-age gauge returns to zero after all requests finish.

## Abort conditions

- Do not force module unload, reboot, or device destruction while requests remain in flight unless the incident commander accepts host-level recovery.
- Stop on kernel oops/panic or evidence of a completion occurring twice.
- Do not extend the deadline merely to silence the alert.

## Recovery and rollback

Restore the failed Provider/RDMA/Backing Store path and confirm request completion. If requests remain stuck, stop workloads and perform controlled host recovery, then run verified I/O before reuse. Roll back to the Local Swap Baseline and retain kernel, status, and VM artifacts for diagnosis.
