# Capacity change

## Preflight

- Capture Provider status/metrics and Consumer placement before the change.
- Verify proposed pool maxima total at most 128 GiB and preserve Host Reserve.
- Confirm every Consumer quota fits the new pool and identify Remote-Only committed assignments.

## Expected output

After a Provider restart, pool maxima match the approved configuration, assigned counters do not exceed maxima, quarantined chunks are zero, and capacity alerts clear. Consumer advertised capacity changes only through explicit device recreation.

## Abort conditions

- Do not shrink below assigned committed capacity.
- Do not reclaim a Committed Remote Chunk from a live Remote-Only device.
- Abort if available host memory would cross Host Reserve or any counter is inconsistent.

## Recovery and rollback

For Backed Mode, drain/upgrade one Provider at a time and validate fallback. Stop and recreate Remote-Only devices before changing committed capacity. Apply the validated Provider config, restart, and verify status before restoring admission. On failure restore the previous config and pool maxima; use the Local Swap Baseline if Consumer capacity cannot be restored.
