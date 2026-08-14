# Authentication failure

## Preflight

1. Compare Consumer identity and current key identifier with the Provider allowlist without printing either PSK.
2. Verify key files are root-owned mode `0600` and the Consumer clock is valid.
3. Capture authentication counters and the Provider structured audit event.

## Expected output

A valid current or next key creates one ready Consumer session. Invalid, expired, or revoked authorization increments authentication failures and closes the session without exposing secret material.

## Abort conditions

- Never paste a PSK into logs, status, metrics, shell history, or tickets.
- Abort rotation if current and next keys do not overlap as planned.
- Do not revoke the only working key before all Consumers reconnect on the new key.

## Recovery and rollback

Follow `psk-rotation-revocation.md`, reload the Provider allowlist, and recreate/reconnect the Consumer as required. Confirm a ready session and no new failures for five minutes. During normal rotation, restore the previous current/next allowlist if validation fails; for emergency revocation, keep the compromised key revoked and move to Backed Mode or the Local Swap Baseline.
