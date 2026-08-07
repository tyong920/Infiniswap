# Infiniswap

Infiniswap lets a Memory Consumer use host DRAM contributed by one or more
Memory Providers over RDMA. The project preserves the original one-sided RDMA
architecture while modernizing it for controlled production use.

The current supported build targets are:

- Ubuntu 22.04 with the Linux 5.15 GA kernel, including Linux 5.15 with
  MLNX_OFED 5.8.
- Ubuntu 24.04 with the Linux 6.8 GA kernel and inbox RDMA.

The current Memory Consumer milestone exposes a Backed Mode Infiniswap Device
that can connect to one authenticated Memory Provider over one configured RC
RDMA Rail. Strict is the production default and waits for both Backing Store and
Remote Memory outcomes; Remote-First remains an explicit latency-oriented
opt-in. Both policies preserve successful Backing Store writes as Local-Only
Data when RDMA fails. Provider operations fall back by the configured deadline,
late completions are ignored, and terminal backing failures enter observable
Backing-Degraded state and reject new writes until the device is recreated.
This code is for controlled Soft-RoCE and block verification, not production
swap cutover. Runtime validation and production operations are tracked
separately in `docs/modernization-plan.md`.

## Build Dependencies

Install the build tools, RDMA development libraries, crypto library, and the GA
kernel headers:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  libibverbs-dev librdmacm-dev libssl-dev \
  linux-headers-generic
```

## Build the Memory Provider

CMake obtains `libibverbs`, `librdmacm`, and `libcrypto` through `pkg-config`.
The project enables `-Wall -Wextra -Wpedantic -Werror`; the following single
command therefore builds with warnings as errors:

```bash
cmake -S infiniswap_daemon -B build/daemon -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build/daemon
```

Run the daemon unit tests with:

```bash
ctest --test-dir build/daemon --output-on-failure
```

The executable is `build/daemon/infiniswap-daemon`.

### Memory Provider Configuration

Memory capacity is configured at runtime rather than compiled into the daemon.
Copy `config/provider-memory.example.conf` and set all values explicitly:

```ini
version = 1
host_reserve_gib = 8
max_opportunistic_gib = 24
max_committed_gib = 8
```

A Remote Chunk is exactly 1 GiB. The two pool maxima may total at most 128 GiB,
the control protocol's per-Provider limit. The Provider reads Linux
`MemAvailable` directly from `/proc/meminfo`; it grows the Opportunistic Pool
only from memory above Host Reserve and reclaims opportunistic capacity when
Provider-local pressure crosses that reserve. Committed assignments are not
reclaimed under local pressure. Allocation, page touching, and RDMA registration
are transactional, so a partial failure does not publish a grant or change pool
accounting.

Authenticated status responses report the capacity currently admissible from
each pool and set the stable `IS_PROTOCOL_STATUS_HEALTHY` flag while the memory
manager is healthy. Pool counters, per-Consumer assignments, rejection reasons,
and cumulative admission/reclaim metrics are also exposed by the memory-manager
interface in `infiniswap_daemon/infiniswap_memory_manager.h`.

### Authenticated Control Protocol

The Memory Provider now accepts only the bounded, endian-stable control frames
specified by `common/infiniswap_protocol.h`. Protocol 1.1 interoperates with
protocol 1.0 by negotiating the lower minor version and their shared optional
capabilities. Major-version mismatches, unknown required capabilities, malformed
lengths, replayed request identifiers, and out-of-order messages produce a
structured error followed by connection teardown. Unversioned native C-struct
messages are intentionally unsupported.

Each Memory Consumer authenticates with an identifier, key identifier, fresh
nonce, and HMAC-SHA256. The allowlist may hold credentials and runtime resource
limits for multiple Consumer identities. The PSK is never transmitted.
Configure the Provider with a root-owned mode-0600 allowlist:

```ini
version = 1

[consumer:consumer-a]
current_key_id = key-2025-01
current_psk_hex = <32-to-64-byte-PSK-as-hex>
next_key_id = key-2025-02
next_psk_hex = <optional-rotation-PSK-as-hex>
next_valid_until_unix = <required-expiry-for-next-key>
max_connections = 1
max_opportunistic_gib = 32
max_committed_gib = 0
revoked = false
```

Start the Provider with the memory configuration and allowlist:

```bash
build/daemon/infiniswap-daemon :: 9400 \
  /etc/infiniswap/provider-memory.conf \
  /etc/infiniswap/consumers.conf
```

Reload the allowlist with `SIGHUP`. Stop the Provider with `SIGINT` or
`SIGTERM`; it stops listening, disconnects active Consumers, and deregisters and
frees every Remote Chunk before exit. `next_valid_until_unix` must be in the
future and no more than 30 days from load time. Authentication must complete
within five seconds. A next-key session is disconnected when its overlap
expires; a Consumer removed from the file, changed to `revoked = true`, or given
changed keys or limits is disconnected during reload. Malformed reloads leave
the active allowlist unchanged. Error strings and protocol error frames contain
no PSKs or authentication tags.

The versioned configuration contracts and examples live under `config/`:

- `consumer.schema.json` defines the current Consumer v2 contract, including
  identity, mode, acknowledgement policy, Backing Store, capacity, Provider
  Failure Deadline, Hot Range scoring, trusted Providers, and explicit swap
  priority. `consumer-v1.schema.json` preserves the previous contract, which
  loads with documented Hot Range defaults.
- `provider.schema.json` defines the current Provider v2 contract for identity,
  listen address, RDMA Rail, Host Reserve, Opportunistic and Committed Pool
  maxima, per-Consumer quotas, and current/next PSK file references.
  `provider-v1.schema.json` preserves v1.
- `provider-directory.schema.json` defines the current static allowlisted
  Provider Directory and its expected capabilities and placement weights.
  `provider-directory-v1.schema.json` preserves v1.
- `status.schema.json` defines the current stable, secret-free JSON status
  contract; `status-v1.schema.json` and `status-v2.schema.json` preserve the
  previous compatibility contracts.

PSK values never belong in JSON. Each referenced PSK must be a non-symlink
regular file owned by root with mode 0600. Validate a Provider configuration on
its intended host so `infiniswapctl` can also verify the configured RDMA device,
port, and NUMA node:

```bash
sudo bin/infiniswapctl validate provider \
  --config /etc/infiniswap/provider.json
```

## Build the Memory Consumer

The module uses standard external-module Kbuild. Set `KDIR` to any prepared
supported kernel header tree; the command neither changes nor prepares that
tree:

```bash
KDIR=/usr/src/linux-headers-5.15.0-187-generic
make -C infiniswap_bd KDIR="$KDIR" modules
```

Use the same command with a Linux 6.8 header path, for example:

```bash
KDIR=/usr/src/linux-headers-6.8.0-137-generic
make -C infiniswap_bd KDIR="$KDIR" modules
```

The wrapper invokes the canonical Kbuild form below, which is also supported
directly:

```bash
make -C "$KDIR" M="$PWD/infiniswap_bd" modules
```

Kbuild output is emitted under `infiniswap_bd/` and ignored by Git. Clean output
for a selected header tree with:

```bash
make -C infiniswap_bd KDIR="$KDIR" clean
```

### RDMA Header Discovery

For inbox RDMA, no additional build flags are needed. For MLNX_OFED, Kbuild
uses the target kernel release reported by the selected header tree to find an
exact installed DKMS tuple under:

```text
/var/lib/dkms/mlnx-ofed-kernel/<version>/<kernel-release>/<architecture>/
```

It then selects the matching generated header and symbol root (normally
`/usr/src/ofa_kernel/<architecture>/<kernel-release>/`); no MLNX_OFED version
is embedded in the build. Set `INFINISWAP_RDMA_ROOT` only for a nonstandard
layout:

```bash
make -C infiniswap_bd KDIR="$KDIR" \
  INFINISWAP_RDMA_ROOT=/path/to/tuple/build modules
```

Kernel API differences are selected from capabilities present in the target
headers, not from broad kernel-version conditionals.

### Control the Memory Consumer

Configfs remains the kernel object-lifecycle seam, but administrators use
`infiniswapctl` rather than writing its attributes directly. The CLI validates
the complete Consumer configuration, Provider Directory, Backing Store, PSK
permissions, capabilities, mode/policy combination, capacity, deadline, and
swap priority before loading the module or creating a configfs object.
Mutations require root; dry runs do not mutate the host.

Copy `config/consumer.example.json` and
`config/provider-directory.example.json` into `/etc/infiniswap`, then adjust the
identity, dedicated Backing Store, one selected Provider, and key path. Provider
hostnames must resolve to exactly one numeric address during preflight so the
kernel RDMA CM route is deterministic. PSK files contain either 32-64 raw bytes
or 64-128 hexadecimal characters; they remain root-owned mode 0600, are passed
through a write-only configfs attribute, and never appear in status. Remote-Only
Mode is represented in the versioned schema but remains rejected until the
Remote-Only milestone is implemented.

The `hot_range` object sets the runtime mapping threshold and the per-request
read/write score weights. These values remain writable while a device is active;
lowering the threshold immediately reevaluates cold ranges. A newly mapped
Remote Chunk starts with no remotely valid sectors, so reads continue to use the
Backing Store until successful remote writes establish validity.

Review the create preflight, then create and activate only that configured
Infiniswap Device:

```bash
bin/infiniswapctl create \
  --config /etc/infiniswap/consumer.json --dry-run
sudo bin/infiniswapctl create \
  --config /etc/infiniswap/consumer.json
```

Creation does not format or enable swap. Formatting is separately preflighted
and requires explicit destructive confirmation; enabling swap requires an
explicit priority:

```bash
bin/infiniswapctl format infiniswap0 --dry-run
sudo bin/infiniswapctl format infiniswap0 --yes
bin/infiniswapctl enable infiniswap0 --priority 100 --dry-run
sudo bin/infiniswapctl enable infiniswap0 --priority 100
```

Human-readable status is the default. `--json` emits deterministic schema
version 3 data covering lifecycle, operational Backing Store state, mode,
policy, swap state, Provider connections, local/remote capacity, Hot Range
scoring, mapped-range count, correctness/failover metrics, and the last kernel
control error. It never includes key identifiers, PSK paths, PSKs, or
authentication tags:

```bash
bin/infiniswapctl status infiniswap0
bin/infiniswapctl status infiniswap0 --json
```

Disable, drain, and destroy remain separate commands. Each supports `--dry-run`
and targets only the named Infiniswap Device; no command disables or
reprioritizes unrelated swap:

```bash
sudo bin/infiniswapctl disable infiniswap0
sudo bin/infiniswapctl drain infiniswap0
sudo bin/infiniswapctl destroy infiniswap0
```

The acknowledgement policy and Provider Failure Deadline are immutable while a
device is online. `drain` removes its block device from userspace and waits for
every accepted request to complete. It refuses an enabled swap device, while
`destroy` refuses an active device, making the required ordering explicit.
The Backing Store rejects regular files, loop devices, read-only devices, and
Infiniswap devices. Activation opens it exclusively and rejects capacities
larger than it or misaligned to its logical block size before exposing
`/dev/infiniswap0`.

For destructive verification in a disposable VM with a spare non-loop block
device, install `fio` and run:

```bash
sudo INFINISWAP_TEST_DESTRUCTIVE=yes \
  INFINISWAP_TEST_BACKING=/dev/vdb \
  INFINISWAP_TEST_MODULE="$PWD/infiniswap_bd/infiniswap.ko" \
  infiniswap_bd/tests/local-backing-test.sh
```

The test rejects unsafe Backing Stores, checks undersized activation and
accepted backing-error completion, runs verified mixed, boundary, buffered, and
flush I/O at multiple queue depths, repeats lifecycle teardown, and
unloads/reloads the empty module. It overwrites the first 64 MiB of the supplied
Backing Store.

For the end-to-end path, prepare a disposable VM with a dedicated 2 GiB Backing
Store and a Soft-RoCE Rail, then build both the Provider and Consumer. The test
performs authenticated RC setup, verified random and mixed one-sided I/O,
Strict and Remote-First completion with independent backing-buffer lifetimes,
cold Local-Only readback, rejected-authentication fallback, runtime threshold
changes, bounded Provider-death/network fallback, delayed completion safety,
Backing-Degraded write rejection, and repeated teardown:

```bash
sudo INFINISWAP_TEST_DESTRUCTIVE=yes \
  INFINISWAP_TEST_BACKING=/dev/vdb \
  INFINISWAP_TEST_MODULE="$PWD/infiniswap_bd/infiniswap.ko" \
  INFINISWAP_TEST_PROVIDER="$PWD/build/daemon/infiniswap-daemon" \
  INFINISWAP_TEST_RDMA_DEVICE=rxe0 \
  INFINISWAP_TEST_RDMA_ADDRESS=192.0.2.20 \
  infiniswap_bd/tests/remote-backed-test.sh
```

The address must belong to the netdev backing the named Soft-RoCE Rail. The test
allocates one 1 GiB Opportunistic Pool chunk and overwrites the first 2 GiB of
the Backing Store.

`setup/install.sh bd` only builds and installs the module. It does not load the
module, create an Infiniswap Device, format swap, or alter active swap.
`setup/install.sh ctl` installs the Python CLI and versioned schemas below
`PREFIX` (default `/usr/local`).

## Continuous Integration

`.github/workflows/build.yml` runs the Linux 5.15 and 6.8 module compile matrix,
builds the Memory Provider with warnings as errors on Ubuntu 22.04 and 24.04,
validates every JSON schema/example and the status compatibility snapshot, and
runs the Provider and `infiniswapctl` tests.

## Research Background

The original Infiniswap design and evaluation appeared at NSDI 2017:
[Infiniswap: Efficient Memory Disaggregation](https://www.usenix.org/conference/nsdi17/technical-sessions/presentation/gu).
