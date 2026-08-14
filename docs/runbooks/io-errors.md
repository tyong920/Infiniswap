# I/O errors

## Preflight

1. Stop new workload admission and capture Consumer status, metrics, `dmesg`, and workload error output.
2. Identify mode, operational state, Backing Store health, Provider state, and in-flight I/O.
3. Check for data mismatch, kernel warning/oops/panic, and repeated completion symptoms.

## Expected output

`io_errors_total` increases only when a block request completes with an error. Backing-Degraded or Remote-Lost explains expected terminal rejection; healthy status with errors requires diagnosis.

## Abort conditions

- Stop all recovery automation on data mismatch, kernel oops/panic, or suspected double completion.
- Do not unload the module with in-flight I/O.
- Do not continue Remote-Only workloads after Remote-Lost.

## Recovery and rollback

Use the matching state runbook, drain and recreate when required, and verify a clean `fio --verify` before restoring workload. The rate alert clears five minutes after the last error. If cause is unresolved, disable Infiniswap and return to the Local Swap Baseline while preserving artifacts.
