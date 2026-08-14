# Admission rejected

## Preflight

1. Capture Consumer and Provider JSON status plus OpenMetrics.
2. Read the Provider last rejection and compare pool capacity with the Consumer quota.
3. Confirm Provider health, Host Reserve, and requested mode/pool pairing.

## Expected output

The Provider `admission_rejections_total` increases and identifies a bounded rejection code. No grant is published and pool assignment counters remain unchanged after a transactional rejection.

## Abort conditions

- Do not retry in a tight loop.
- Do not bypass Consumer quotas, Host Reserve, or mode/pool validation.
- Stop if rejection is cleanup failure or creates quarantined Remote Chunks.

## Recovery and rollback

Correct capacity, quota, configuration, or Provider health, then allow a bounded reconnect/remap attempt. The event-rate alert clears five minutes after the last rejection. Roll back the capacity/configuration change if counters become inconsistent; keep Backed Mode on the Backing Store meanwhile.
