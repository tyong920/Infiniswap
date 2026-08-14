# PSK rotation and revocation

## Preflight

- Identify one Consumer and its Provider allowlist entry; never display key material.
- Verify new key files are 32-64 bytes, root-owned, mode `0600`, and delivered out of band.
- Record a change identifier and confirm the bounded current/next overlap window.

## Expected output

During rotation, both authorized key generations work only for the intended Consumer. After reload and Consumer recreation/reconnect, Provider status shows a ready session and authentication counters stop increasing. Emergency revocation disconnects the target Consumer independently.

## Abort conditions

- Abort normal rotation before revocation if the Consumer has not authenticated with the new key.
- Never put a PSK in status, metrics, audit details, shell arguments, or tickets.
- Do not roll back to a compromised key during emergency revocation.

## Recovery and rollback

Install the next key on both hosts, reload the Provider allowlist with `SIGHUP`, recreate/reconnect the Consumer, then promote and remove the old key. Record `infiniswapctl audit key-rotated --subject <consumer> --change-id <id> --outcome completed`. For normal failure, restore the previous current/next metadata and reconnect. For compromise, use `key-revoked`, keep the old key revoked, and move workloads to Backed Mode or the Local Swap Baseline.
