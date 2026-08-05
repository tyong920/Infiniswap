# Infiniswap Production Modernization Plan

**Status:** Confirmed design; implementation not started
**Program:** [GitHub issue #1](https://github.com/tyong920/Infiniswap/issues/1)
**Decision record:** `CONTEXT.md` and `docs/adr/0001-*.md` through `docs/adr/0015-*.md`

## Objective

Modernize Infiniswap into a host-wide production swap implementation for the controlled internal GPU/RDMA cluster. The first production target is `ty-gpu-02` as one Memory Consumer connected to multiple Memory Providers.

Production cutover is conditional: a candidate mode must pass correctness, stability, operational, and workload performance gates against the existing NVMe Local Swap Baseline. Engineering completion without a passing performance candidate does not authorize swap cutover.

## Supported platforms

| Target | Kernel/RDMA stack | Required evidence |
| --- | --- | --- |
| Ubuntu 22.04 | Linux 5.15 GA, including `ty-gpu-02` with MLNX_OFED 5.8 | Build, VM regression, physical RDMA certification, canary |
| Ubuntu 24.04 | Linux 6.8 GA with inbox RDMA | Build, VM regression, physical RDMA certification |

Each new Ubuntu kernel ABI is held from production until DKMS compilation, VM regression, and a short canary pass. Linux 3.x/4.x and arbitrary post-5.15 kernel lines are not supported.

## First-GA boundaries

Included:

- Backed Mode with Strict as the production default and explicit Remote-First opt-in.
- Remote-Only Mode on Remote-Only-Eligible hosts.
- One Consumer connected to multiple Providers.
- One explicit, NUMA-aware RDMA Rail per Provider connection.
- A static, versioned, allowlisted Provider Directory.
- Debian/DKMS/systemd packaging, metrics, alerts, upgrades, rollback, fault injection, soak, and canary.

Excluded:

- GPU HBM expansion.
- Cross-Provider replication or migration.
- Multi-rail aggregation.
- Dynamic Provider discovery.
- Multiple production Consumers sharing one Provider.
- Compression, erasure coding, GUI work, and legacy binary compatibility.

## Delivery rules

1. Keep Linux 5.15 and 6.8 compile gates green from the first implementation change.
2. Deliver vertical, runnable slices; do not combine the kernel port, protocol rewrite, and all failure modes into one untestable change.
3. Put complex behavior behind small interfaces: protocol codec, Provider memory manager, Consumer lifecycle/state machine, backing adapter, and RDMA adapter.
4. Every accepted block request completes exactly once. Every error path unwinds memory registrations, queue resources, threads, and state.
5. Production-affecting commands are explicit, preflighted, auditable, and reversible. No tool modifies unrelated swap.
6. New behavior requires tests at the same seam used by callers. Avoid test-only alternate implementations of production behavior.
7. ADRs remain the source of truth for hard-to-reverse decisions; issues own implementation scope and current status.

## Dependency graph

```text
#2 Build foundation
|-- #3 Versioned/authenticated protocol
|   `-- #5 Provider memory manager
|-- #4 Consumer local block path
|   `-- #6 Control and status contracts
|       `-- #7 Single-Provider Backed Remote-First
|           `-- #8 Strict and bounded Backed failure handling
|               `-- #9 Remote-Only and Committed Pool
|                   `-- #10 Multi-Provider placement
|                       `-- #11 VM validation and fault injection
|                           `-- #12 Production operations
|                               `-- #13 Packaging and upgrades
|                                   `-- #14 Physical certification
|                                       `-- #15 ty-gpu-02 canary/cutover
```

GitHub native dependencies are authoritative where the simplified diagram omits additional blockers.

## Phases

### Phase 0 - Build and protocol foundation

- [#2 Establish Kbuild, CMake, and the Ubuntu 5.15/6.8 build matrix](https://github.com/tyong920/Infiniswap/issues/2)
- [#3 Define and test the versioned authenticated control protocol](https://github.com/tyong920/Infiniswap/issues/3)

Exit gate:

- Consumer module compiles against supported inbox and MLNX_OFED layouts without modifying kernel header trees.
- Provider builds with CMake and strict warnings.
- CI covers both kernel lines and protocol malformed-input/golden-vector tests.
- No native C struct is sent on the wire.

### Phase 1 - Correct local block device

- [#4 Port the Consumer block-device lifecycle and local Backing Store path](https://github.com/tyong920/Infiniswap/issues/4)

Exit gate:

- The modern `blk-mq` device passes verified local Backing Store I/O on 5.15 and 6.8 with RDMA disabled.
- Create, drain, destroy, unload, and all request completion paths are clean.
- Device creation, formatting, and swap activation remain separate operations.

### Phase 2 - First end-to-end Backed path

- [#5 Implement Provider memory pools, quotas, and admission control](https://github.com/tyong920/Infiniswap/issues/5)
- [#6 Establish infiniswapctl, configuration schemas, and stable status contracts](https://github.com/tyong920/Infiniswap/issues/6)
- [#7 Deliver single-Provider Backed Remote-First RDMA I/O](https://github.com/tyong920/Infiniswap/issues/7)

Exit gate:

- One authenticated Consumer and one Provider complete verified one-sided RDMA I/O over Soft-RoCE.
- Backing and remote completions can arrive in either order without double completion or resource leaks.
- Cold/unmapped data remains correct locally; unsafe configuration fails before kernel mutation.

### Phase 3 - Backed production semantics

- [#8 Add Strict, Local-Only, Backing-Degraded, and bounded failover](https://github.com/tyong920/Infiniswap/issues/8)

Exit gate:

- Strict is the production default.
- The full local/remote completion matrix is tested.
- Provider death falls back by the configured Provider Failure Deadline, default two seconds.
- Late completions are harmless; Backing-Degraded rejects new writes and is observable.

### Phase 4 - Remote-Only

- [#9 Add Remote-Only, Committed Pool, and Remote-Lost](https://github.com/tyong920/Infiniswap/issues/9)

Exit gate:

- Full advertised capacity is reserved atomically before activation.
- Committed Remote Chunks are never opportunistically reclaimed.
- Any Provider loss transitions the entire device to Remote-Lost by the deadline.
- Mixed-recoverability hosts are rejected for production Remote-Only activation.

### Phase 5 - Multi-Provider and virtual validation

- [#10 Add Power-of-d multi-Provider placement and Hot Range mapping](https://github.com/tyong920/Infiniswap/issues/10)
- [#11 Automate two/three-VM Soft-RoCE validation and fault injection](https://github.com/tyong920/Infiniswap/issues/11)

Exit gate:

- One Consumer uses two and three allowlisted Providers with deterministic test placement and production-random Power-of-d placement.
- Ubuntu 22.04 and 24.04 guests complete the full correctness/failure matrix.
- Each supported kernel completes a 24-hour VM soak without data mismatch, kernel oops/panic, unexplained hung I/O, or leaked resources.

VM host budgets:

- Default: at most 6 vCPU, 48 GiB RAM, and 100 GiB sparse disk.
- `--large`: at most 16 vCPU, 128 GiB RAM, and 160 GiB sparse disk.
- Guest RXE only; no host Infiniswap, host RDMA changes, hugepages, or host swap changes.

### Phase 6 - Production operations and delivery

- [#12 Deliver production metrics, alerts, and operator runbooks](https://github.com/tyong920/Infiniswap/issues/12)
- [#13 Package DKMS, Debian, systemd, signing, upgrade, and rollback](https://github.com/tyong920/Infiniswap/issues/13)

Exit gate:

- Every degraded/terminal state has stable status, metrics, an exercised alert, and a runbook.
- Fresh install, upgrade, downgrade, kernel ABI update, PSK rotation, Consumer drain, and rollback pass on both supported Ubuntu releases.
- No package action implicitly activates swap or modifies unrelated swap.

### Phase 7 - Physical certification and production cutover

- [#14 Certify physical RDMA tuples and the frozen GPU workload](https://github.com/tyong920/Infiniswap/issues/14)
- [#15 Canary and cut over ty-gpu-02 with demonstrated rollback](https://github.com/tyong920/Infiniswap/issues/15)

Exit gate:

- Each physical tuple completes a 72-hour block/swap soak with the full fault matrix.
- A frozen host-DRAM-heavy GPU workload completes at least five valid runs per candidate configuration.
- A candidate improves the median primary metric by at least 20%, its 95% confidence interval excludes no improvement, and no-pressure regression is at most 5%.
- `ty-gpu-02` completes a seven-day non-critical canary with zero data mismatch, kernel oops/panic, unexplained hung I/O, or missed critical alert.
- Local Swap Baseline rollback is demonstrated before cutover is declared complete.

## External prerequisites

These are release blockers rather than coding tasks:

- Human authorization for two physical RDMA staging nodes and maintenance windows.
- An Ubuntu 24.04/Linux 6.8/inbox-RDMA physical certification tuple.
- A dedicated block device or LVM LV for production Backed Mode on `ty-gpu-02`.
- A frozen production GPU workload, dataset, parameters, local DRAM limit, effective swap capacity, and primary metric before Phase 7.
- Production module-signing keys and PSK provisioning/rotation ownership.
- Host-owner approval that Infiniswap is host-wide swap; Remote-Only additionally requires whole-host eligibility.

## Program completion

Close [#1](https://github.com/tyong920/Infiniswap/issues/1) only when every child issue is closed and all Phase 7 evidence is attached. If engineering gates pass but no candidate clears the performance gate, retain the Local Swap Baseline and close the program without production cutover only through an explicit follow-up decision.
