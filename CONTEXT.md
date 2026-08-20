# Infiniswap

Infiniswap lets hosts under memory pressure use host memory contributed by other machines over an RDMA network.

## Language

**Memory Consumer**:
A host whose workloads use memory contributed by one or more Memory Providers.
_Avoid_: Client, block-device host, M1

**Memory Provider**:
A host that contributes part of its host DRAM for use by Memory Consumers.
_Avoid_: Server, daemon host, M2

**Remote Memory**:
Host DRAM contributed by a Memory Provider and made available to Memory Consumers. It does not include GPU device memory, VRAM, or HBM.
_Avoid_: GPU memory, VRAM, HBM

**Backing Store**:
Local block storage used by a Memory Consumer to retain a recoverable copy of data placed in Remote Memory when backing is enabled.
_Avoid_: Backup disk, physical disk

**Backed Mode**:
An operating mode in which a Memory Consumer maintains a recoverable local copy in a Backing Store.
_Avoid_: Safe mode, disk mode

**Remote-Only Mode**:
An operating mode without a Backing Store. Loss of any Memory Provider holding its data makes the entire Infiniswap Device unavailable.
_Avoid_: Performance mode, unsafe mode

**Remote Chunk**:
A fixed 1 GiB unit in which Remote Memory is allocated, placed, and evicted.
_Avoid_: Slab, memory region

**Infiniswap Device**:
A logical swap resource through which a Memory Consumer uses Remote Memory. Its operating mode is chosen when the resource is created.
_Avoid_: NBDX device, remote disk

**Committed Remote Chunk**:
A Remote Chunk assigned to a Remote-Only Mode device that its Memory Provider must not evict while the device uses it.
_Avoid_: Pinned chunk, static chunk

**Remote-First Policy**:
A Backed Mode acknowledgement policy that reports a write after its Remote Memory write completes and its Backing Store write has been submitted.
_Avoid_: Fast mode, async mode

**Strict Policy**:
A Backed Mode acknowledgement policy that waits for both writes to finish before reporting. A successful Backing Store write preserves correctness when the Remote Memory write fails, leaving Local-Only Data.
_Avoid_: Safe mode, synchronous mode

**Remote I/O Transaction**:
The full lifecycle of one accepted read or write that uses Remote Memory and, in Backed Mode, may also use the Backing Store. A request may be acknowledged before its Remote I/O Transaction settles under the Remote-First Policy; the transaction settles only after every participating path reaches a terminal result and no transaction-owned resources remain.
_Avoid_: Block request, RDMA operation, completion callback

**Backing-Degraded**:
A Backed Mode device state in which the Backing Store can no longer maintain the promised recovery copy. New writes are rejected until an operator restores a valid backed device.
_Avoid_: Remote-Only Mode, warning state

**Local-Only Data**:
Data whose current valid copy is in the Backing Store but not in Remote Memory.
_Avoid_: Lost data, failed data

**Opportunistic Pool**:
Remote Memory reserved for Backed Mode devices that a Memory Provider may reclaim through the normal eviction protocol.
_Avoid_: Dynamic pool, shared pool

**Committed Pool**:
Remote Memory reserved for Remote-Only Mode devices that a Memory Provider must not reclaim while assigned.
_Avoid_: Static pool, pinned pool

**Host Reserve**:
Memory that a Memory Provider keeps unavailable for Remote Memory so its own workloads retain an explicit safety margin.
_Avoid_: Free-memory threshold, headroom

**Remote-Lost**:
A terminal Remote-Only Mode device state entered when any Memory Provider holding its data is lost. The device rejects all further I/O and must be recreated.
_Avoid_: Disconnected, degraded

**Remote-Only-Eligible Host**:
A Memory Consumer whose every swap-eligible workload can recover through restart, recomputation, or an application-level checkpoint.
_Avoid_: Trusted host, test host

**Power-of-d Placement**:
A placement policy that samples a configured number of healthy Memory Providers and assigns a Remote Chunk to the sampled Provider with the most available capacity.
_Avoid_: Random placement, least-loaded placement

**Hot Range**:
A 1 GiB Backed Mode address range whose runtime-configurable, read/write-weighted activity score exceeds the mapping threshold.
_Avoid_: Active chunk, frequently used memory

**Local Swap Baseline**:
The existing host-local swap implementation used as the production performance comparator and retained offline as the initial rollback path after cutover.
_Avoid_: Fallback swap, old swap

**Provider Failure Deadline**:
The bounded interval a Memory Consumer waits for a Provider operation before Backed Mode falls back or Remote-Only Mode enters Remote-Lost.
_Avoid_: Infinite retry, network timeout

**RDMA Rail**:
A configured RDMA network path used by a Memory Consumer to communicate with one Memory Provider, including its device, address, and NUMA locality.
_Avoid_: NIC, interface

**Provider Directory**:
The versioned, allowlisted set of Memory Providers a Memory Consumer may use, including each Provider's identity, address, RDMA Rail, expected capabilities, and placement weight.
_Avoid_: Portal list, server list, service discovery
