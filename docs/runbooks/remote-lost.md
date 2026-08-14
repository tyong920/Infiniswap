# Remote-Lost

## Preflight

1. Confirm `remote-lost` in both JSON operational and connection state.
2. Identify affected restartable/checkpointed workloads and stop new workload admission.
3. Verify the host is still Remote-Only-Eligible and sufficient Provider committed capacity is healthy.

## Expected output

A Remote-Lost device rejects all I/O and never reconnects in place. `remote_lost_transitions_total` is nonzero. The alert remains active until the device is removed and recreated.

## Abort conditions

- Do not treat a Provider reconnect as data recovery.
- Do not recreate before affected workloads are stopped and their recovery path is confirmed.
- Abort if any swap-eligible workload is not recoverable; use Backed Mode instead.

## Recovery and rollback

Disable swap if still listed, drain/stop the terminal device, recover workloads through restart, recomputation, or checkpoint, then recreate against healthy committed capacity. Reformat and enable only after status is healthy. If eligibility or capacity is uncertain, switch to Backed Mode or the Local Swap Baseline.
