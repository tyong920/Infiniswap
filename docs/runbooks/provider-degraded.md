# Provider degraded

## Preflight

1. Fetch `infiniswapctl provider-status` and `infiniswapctl provider-metrics` locally on the Provider.
2. Record pool allocation, quarantine, Host Reserve, active Consumers, and the last rejection.
3. Confirm another healthy Provider exists before a Backed Mode rolling restart.

## Expected output

A healthy Provider reports `healthy`, zero quarantined Remote Chunks, and a responsive `/healthz`. Degraded status remains active after cleanup or accounting failures until the daemon is safely restarted.

## Abort conditions

- Do not restart a Provider serving a live Remote-Only Mode device.
- Stop if registered memory cannot be reclaimed or active connection count does not reach zero.
- Do not reduce Host Reserve to hide an accounting failure.

## Recovery and rollback

Drain affected Backed Mode connections, stop the daemon, correct memory accounting or registration failures, and restart it. Confirm `/healthz` returns 200 and pool counters are coherent. If it remains degraded, keep it out of the Provider Directory and restore the previous daemon build/configuration.
