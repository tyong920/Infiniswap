# Provider rolling upgrade

## Preflight

- Confirm Consumer mode and placement for every Provider.
- Verify remaining Providers plus the Backing Store are healthy and alerts are quiet.
- Validate the new daemon build/configuration and preserve the previous package/config.

## Expected output

In Backed Mode, one Provider disconnects and affected reads fall back. Before touching the next Provider, the upgraded Provider is healthy and the Consumer has been drained/recreated so all configured Providers are connected again. No I/O errors, Backing-Degraded, hung requests, or data mismatches occur.

## Abort conditions

- Remote-Only Mode is not eligible for an in-place Provider rolling upgrade; stop/recreate its device first.
- Abort the roll on any terminal/degraded Consumer state, failed `/healthz`, or uncleared disconnect alert.
- Never upgrade two Providers concurrently in the first production topology.

## Recovery and rollback

Remove one Provider from admission, then run `infiniswapctl upgrade provider`
with the reviewed target and rollback `.deb` artifacts. The command restarts one
Provider, requires `/healthz`, and reinstalls the previous artifact if health
fails. Drain and recreate the Backed Mode Consumer so the upgraded Provider
rejoins, then run verified I/O and require all Providers connected before
continuing. The command refuses Remote-Only Mode; stop/recreate that device
first. If the Consumer remains unhealthy, restore the Local Swap Baseline.
