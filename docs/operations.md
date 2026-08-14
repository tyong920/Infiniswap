# Production operations

## Semantics operators must preserve

- **Backed Mode** keeps a recoverable copy in a local Backing Store. Provider loss should fall back by the Provider Failure Deadline, but Backing-Degraded rejects new writes and requires device recreation.
- **Strict Policy** is the Backed Mode default. A write succeeds only after both Remote Memory and Backing Store writes complete. A valid local completion can preserve Local-Only Data if the remote write fails.
- **Remote-First Policy** is an explicit Backed Mode opt-in. A write may be acknowledged after the remote write completes and the Backing Store write is submitted; a later backing failure transitions to Backing-Degraded.
- **Remote-Only Mode** has no Backing Store and is allowed only on a Remote-Only-Eligible Host. Loss of a Provider holding data causes terminal Remote-Lost; reconnect is not recovery.
- **Local Swap Baseline** is independent host-local swap. It is the performance comparator and initial rollback path, not an automatic fallback managed by Infiniswap.

Strict and Remote-First are acknowledgement policies inside Backed Mode; they are not additional device modes.

## Observability interfaces

- Consumer human/JSON: `infiniswapctl status <device> [--json]` (JSON schema v6).
- Consumer OpenMetrics: `infiniswapctl metrics <device>` (metrics schema v1).
- Active Consumer alerts: `infiniswapctl alerts <device> [--json]`.
- Provider human/JSON: `infiniswapctl provider-status [--json]`; the daemon serves loopback `/status`, `/metrics`, and `/healthz` on port 9401 by default.
- Provider OpenMetrics: `infiniswapctl provider-metrics` (metrics schema v1).
- Set `INFINISWAP_PROVIDER_ID` to the Provider Directory identity and `INFINISWAP_OBSERVABILITY_PORT` to the approved local port; `0` disables the endpoint only for isolated tests.

- Production-changing CLI commands append schema-versioned JSONL to `/var/log/infiniswap/audit.jsonl`; Provider key reloads emit matching structured events to the system journal.

Only bounded device, Provider, Consumer, pool, mode, policy, and state labels are emitted. Status and metrics exclude PSKs, key identifiers, authentication tags, addresses, paths to secrets, request identifiers, sectors, and per-I/O labels.

## Runbook index

Alerts: [Provider disconnected](runbooks/provider-disconnected.md), [deadline expired](runbooks/provider-deadline-expired.md), [Provider degraded](runbooks/provider-degraded.md), [Backing-Degraded](runbooks/backing-degraded.md), [Remote-Lost](runbooks/remote-lost.md), [pool pressure](runbooks/pool-pressure.md), [admission rejected](runbooks/admission-rejected.md), [authentication failure](runbooks/authentication-failure.md), [I/O errors](runbooks/io-errors.md), and [hung requests](runbooks/hung-requests.md).

Operations: [normal start/stop](runbooks/normal-start-stop.md), [capacity change](runbooks/capacity-change.md), [PSK rotation/revocation](runbooks/psk-rotation-revocation.md), [Provider rolling upgrade](runbooks/provider-rolling-upgrade.md), [Consumer drain](runbooks/consumer-drain.md), and [rollback](runbooks/rollback.md).
