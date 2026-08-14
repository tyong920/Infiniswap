# Pool pressure

## Preflight

1. Run `infiniswapctl provider-status` and record both pool maxima, assigned/available Remote Chunks, Host Reserve, and active Consumers.
2. Check host available memory and workload demand.
3. Determine whether pressure is Opportunistic Pool reclaim or Committed Pool exhaustion.

## Expected output

The alert fires after admissible capacity remains below ten percent for five minutes. Opportunistic assignments may be reclaimed through normal eviction; committed assignments remain protected.

## Abort conditions

- Never reclaim Committed Remote Chunks from a live Remote-Only Mode device.
- Do not lower Host Reserve without an approved capacity change.
- Stop if cleanup creates quarantined chunks or Provider-Degraded.

## Recovery and rollback

Reduce new admission, add Provider capacity, or perform the approved capacity-change runbook. Confirm available capacity exceeds the threshold for five minutes. If pressure cannot be relieved, drain Backed Mode Consumers or restore the prior pool limits; stop/recreate Remote-Only devices before changing their committed capacity.
