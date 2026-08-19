# Infiniswap

Infiniswap lets a Memory Consumer use host DRAM contributed by one or more
Memory Providers over RDMA. The project preserves the original one-sided RDMA
architecture while modernizing it for controlled production use.

The current supported build targets are:

- Ubuntu 22.04 with the Linux 5.15 GA kernel, including Linux 5.15 with
  MLNX_OFED 5.8.
- Ubuntu 24.04 with the Linux 6.8 GA kernel and inbox RDMA.

The current Memory Consumer milestone exposes Backed Mode and Remote-Only Mode
Infiniswap Devices that connect to one authenticated Memory Provider over one
configured RC RDMA Rail. Strict is the production Backed Mode default and waits
for both Backing Store and Remote Memory outcomes; Remote-First remains an
explicit latency-oriented opt-in. Both policies preserve successful Backing
Store writes as Local-Only Data when RDMA fails. Provider operations fall back
by the configured deadline, late completions are ignored, and terminal backing
failures enter observable Backing-Degraded state. Remote-Only Mode is gated by
an explicit Remote-Only-Eligible Host declaration, reserves its complete
capacity from the Committed Pool before exposing a block device, and enters
terminal Remote-Lost after Provider loss. This code is for controlled Soft-RoCE
and block verification, not production swap cutover. Runtime validation and
production operations are tracked separately in `docs/modernization-plan.md`.

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

## Debian Packages and Safe Upgrades

Builds produce separate `infiniswap-dkms`, `infiniswap-provider`, and
`infiniswapctl` Debian packages. The Provider unit is installed disabled and
stopped; no package action creates an Infiniswap Device, formats storage,
activates swap, or modifies unrelated swap. DKMS supports the validated Linux
5.15 and 6.8 GA lines and offers an optional operator-key signing hook.

Consumer upgrades use capacity-aware `infiniswapctl upgrade preflight` before
the named swap is disabled and drained. Provider rolling upgrades are
Provider-first for Backed Mode and are rejected for Remote-Only Mode until the
device is stopped and recreated. Configuration migration and rollback are
explicit, artifact-backed operations. See `docs/packaging.md` for package build,
fresh install, upgrade, downgrade, purge/reinstall, signing, and kernel-ABI gate
procedures.

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

- `consumer.schema.json` defines the current Consumer v3 contract, including
  the Remote-Only-Eligible Host declaration, mode, acknowledgement policy,
  Backing Store, capacity, Provider Failure Deadline, Hot Range scoring,
  trusted Providers, and explicit swap priority. `consumer-v1.schema.json` and
  `consumer-v2.schema.json` preserve the previous Backed Mode contracts; v1
  loads with documented Hot Range defaults.
- `provider.schema.json` defines the current Provider v2 contract for identity,
  listen address, RDMA Rail, Host Reserve, Opportunistic and Committed Pool
  maxima, per-Consumer quotas, and current/next PSK file references.
  `provider-v1.schema.json` preserves v1.
- `provider-directory.schema.json` defines the current static allowlisted
  Provider Directory and its expected capabilities and placement weights.
  `provider-directory-v1.schema.json` preserves v1.
- `status.schema.json` defines the current stable, secret-free JSON status
  contract; `status-v1.schema.json` through `status-v3.schema.json` preserve
  the previous compatibility contracts.

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
identity, dedicated Backing Store, one selected Provider, and key path. For
Remote-Only Mode, start from `config/consumer-remote-only.example.json`; setting
`identity.remote_only_eligible` to true declares that every swap-eligible
workload on the host can recover after Remote-Lost. The CLI rejects Remote-Only
Mode on earlier schema versions, a false declaration, a Backing Store, or a
Provider without Remote-Only and Committed Pool capabilities.

Provider hostnames must resolve to exactly one numeric address during preflight
so the kernel RDMA CM route is deterministic. PSK files contain either 32-64
raw bytes or 64-128 hexadecimal characters; they remain root-owned mode 0600,
are passed through a write-only configfs attribute, and never appear in status.

The `hot_range` object sets the runtime mapping threshold and the per-request
read/write score weights. These values remain writable while a device is active;
lowering the threshold immediately reevaluates cold ranges. A newly mapped
Remote Chunk starts with no remotely valid sectors, so reads continue to use the
Backing Store until successful remote writes establish validity.

Remote-Only activation is synchronous: the Provider must atomically grant one
Committed Remote Chunk for every 1 GiB of advertised capacity before
`/dev/infiniswap0` appears. Those assignments are excluded from normal pressure
eviction. Reads are accepted only after successful writes establish remote
validity; there is no Backing Store fallback. While any Infiniswap Device is
active, an authenticated status heartbeat uses one third of the Provider
Failure Deadline for its interval and one third for its response. At the full
failure deadline, the Provider closes the silent Memory Consumer's work queue,
waits for in-flight completions to drain, and immediately returns all of that
session's Remote Chunks to its pools without waiting for a peer disconnect
acknowledgement; the final third provides scheduler margin before that
reclamation.

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
version 6 data covering lifecycle, operational state (including Remote-Lost),
mode, policy, deadlines, swap state, per-Provider connections/capacity/errors,
local/remote capacity, Remote Chunks, in-flight I/O, correctness/failover
metrics, and the last kernel control error. OpenMetrics schema version 1 and
active alert evaluation use the same status snapshot. None of these interfaces
includes key identifiers, PSK paths, PSKs, or authentication tags:

```bash
bin/infiniswapctl status infiniswap0
bin/infiniswapctl status infiniswap0 --json
bin/infiniswapctl metrics infiniswap0
bin/infiniswapctl alerts infiniswap0 --json
bin/infiniswapctl provider-status
bin/infiniswapctl provider-status --json
bin/infiniswapctl provider-metrics
```

The Memory Provider serves loopback-only `/status`, `/metrics`, and `/healthz`
on port 9401 by default. Set `INFINISWAP_PROVIDER_ID` to its Provider Directory
identity. Baseline alert rules live in `monitoring/infiniswap-alerts.yml`; mode
semantics and alert/operation runbooks are indexed in `docs/operations.md`.

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
changes, bounded Provider-death fallback, delayed completion safety,
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
the Backing Store. Set
`INFINISWAP_TEST_CASE=remote-first-backing-failure` to run only the focused
Remote-First fault case. That case reads the written payload back from Remote
Memory. This distinguishes an application-visible write error with the payload
present in Remote Memory from a failure where the remote payload is absent.

A silent-network fault needs an external Provider because self-SoftRoCE traffic
never reaches the local netdev's qdisc or firewall. Start the external Provider
with the two-VM configuration shown below, changing both Opportunistic Pool
limits to 1 GiB and both Committed Pool limits to zero. Then run this focused
case on the Consumer, replacing the PSK and Provider address:

```bash
sudo INFINISWAP_TEST_DESTRUCTIVE=yes \
  INFINISWAP_TEST_CASE=network-fault \
  INFINISWAP_TEST_EXTERNAL_PROVIDER=yes \
  INFINISWAP_TEST_PSK_HEX="<PSK printed by Provider setup>" \
  INFINISWAP_TEST_FAULT_MODE=roce-iptables \
  INFINISWAP_TEST_BACKING=/dev/vdb \
  INFINISWAP_TEST_MODULE="$PWD/infiniswap_bd/infiniswap.ko" \
  INFINISWAP_TEST_RDMA_DEVICE=rxe0 \
  INFINISWAP_TEST_RDMA_ADDRESS="<Provider IPv4 address>" \
  INFINISWAP_TEST_PROVIDER_PORT=19401 \
  infiniswap_bd/tests/remote-backed-test.sh
```

The focused test requires an observed Provider timeout before accepting the
`degraded` transition, preventing successful self-SoftRoCE I/O from being
misclassified as network-failure fallback.

A separate Remote-Only test needs no Backing Store. Its default local-Provider
path verifies the host gate, atomic full-capacity admission, repeated Committed
Pool reserve/release cycles, and verified one-sided I/O. It does not claim a
silent-network transition because self-SoftRoCE traffic bypasses the local
netdev's qdisc:

```bash
sudo INFINISWAP_TEST_DESTRUCTIVE=yes \
  INFINISWAP_TEST_MODULE="$PWD/infiniswap_bd/infiniswap.ko" \
  INFINISWAP_TEST_PROVIDER="$PWD/build/daemon/infiniswap-daemon" \
  INFINISWAP_TEST_RDMA_DEVICE=rxe0 \
  INFINISWAP_TEST_RDMA_ADDRESS=192.0.2.20 \
  infiniswap_bd/tests/remote-only-test.sh
```

For a two-VM topology, prepare and start the external Provider first:

```bash
psk="$(openssl rand -hex 32)"
printf 'Use this PSK on the Consumer: %s\n' "$psk"
cat >/tmp/provider-memory.conf <<'EOF'
version = 1
host_reserve_gib = 1
max_opportunistic_gib = 0
max_committed_gib = 1
EOF
cat >/tmp/consumers.conf <<EOF
version = 1

[consumer:consumer-test]
current_key_id = key-test
current_psk_hex = $psk
max_connections = 1
max_opportunistic_gib = 0
max_committed_gib = 1
revoked = false
EOF
chmod 0600 /tmp/consumers.conf
sudo chown root:root /tmp/provider-memory.conf /tmp/consumers.conf
sudo prlimit --memlock=unlimited:unlimited -- \
  "$PWD/build/daemon/infiniswap-daemon" :: 19401 \
  /tmp/provider-memory.conf /tmp/consumers.conf
```

Then run the focused reconnect test on the Consumer, replacing the PSK and
Provider address:

```bash
sudo INFINISWAP_TEST_DESTRUCTIVE=yes \
  INFINISWAP_TEST_CASE=reconnect \
  INFINISWAP_TEST_MODULE="$PWD/infiniswap_bd/infiniswap.ko" \
  INFINISWAP_TEST_EXTERNAL_PROVIDER=yes \
  INFINISWAP_TEST_PSK_HEX="<PSK printed by Provider setup>" \
  INFINISWAP_TEST_RDMA_DEVICE=rxe0 \
  INFINISWAP_TEST_RDMA_ADDRESS="<Provider IPv4 address>" \
  infiniswap_bd/tests/remote-only-test.sh
```

This focused case reserves and releases one Committed Remote Chunk, then
immediately repeats activation. It verifies that graceful disconnect completion
prevents the replacement session from being rejected as a stale RDMA
connection.

To test the Remote-Lost network transition, run the focused fault case:

```bash
sudo INFINISWAP_TEST_DESTRUCTIVE=yes \
  INFINISWAP_TEST_CASE=network-fault \
  INFINISWAP_TEST_MODULE="$PWD/infiniswap_bd/infiniswap.ko" \
  INFINISWAP_TEST_EXTERNAL_PROVIDER=yes \
  INFINISWAP_TEST_PSK_HEX="<PSK printed by Provider setup>" \
  INFINISWAP_TEST_FAULT_MODE=netem \
  INFINISWAP_TEST_RDMA_DEVICE=rxe0 \
  INFINISWAP_TEST_RDMA_NETDEV="<netdev backing rxe0>" \
  INFINISWAP_TEST_RDMA_ADDRESS="<Provider IPv4 address>" \
  infiniswap_bd/tests/remote-only-test.sh
```

The network-fault test reserves one Committed Remote Chunk and requires an
observed Provider timeout before it waits for both device states to become
Remote-Lost. It also verifies the transition count and explicit read/write
failure. `netem` briefly interrupts other Consumer traffic on the same netdev.
Set `INFINISWAP_TEST_FAULT_MODE=roce-iptables` to drop only UDP/4791 traffic to
an IPv4 Provider when the management connection must remain unaffected.

### Automated KVM validation

`tests/vm/run` is the release-gate harness for Ubuntu 22.04/Linux 5.15 and
Ubuntu 24.04/Linux 6.8. Its default invocation runs the two-VM topology (one
Memory Consumer and one Memory Provider) and three-VM topology (one Consumer
and two Providers), including a 24-hour verified-I/O soak for every matrix
entry:

```bash
tests/vm/run
```

The host needs x86-64 KVM access, QEMU, `qemu-img`, `cloud-localds`, OpenSSH,
`nice`, and `ionice`. On Ubuntu, the non-base packages are available as
`qemu-system-x86 qemu-utils cloud-image-utils openssh-client`. Preflight runs
before cloud-image download or VM startup and requires the selected aggregate
profile plus a 2 GiB host-memory and 8 GiB cache/artifact reserve to fit. The
default profile requests and caps 6 vCPU, 48 GiB RAM, and 100 GiB sparse disk.
`--large` requests and caps 16 vCPU, 128 GiB RAM, and 160 GiB sparse disk.

The harness verifies Ubuntu's published SHA-256 checksum before caching each
cloud image. QEMU runs at nice level 10 with idle I/O priority, KVM, ordinary
anonymous guest RAM (no hugepages), SLIRP management networking, and private
localhost QEMU socket networks for guest RXE. It never passes through a host
RDMA device and snapshots host swap, protected RDMA/Infiniswap modules, and
image attachments before and after the run. All module, RXE, Backing Store,
and swap operations occur inside disposable guests.

Each matrix entry builds from the current checkout and exercises Backed and
Remote-Only verified `fio`, multi-Provider placement, guest-only swap pressure,
normal device shutdown, Provider `SIGKILL`, RXE packet interruption, a
Device Mapper Backing Store error, Consumer reboot, safe module reload, and the
soak. Data mismatch, Provider deadline failure, unexpected I/O success, kernel
warning/oops/panic, hung I/O, cleanup failure, or leaked QEMU process/socket/disk
fails the machine-readable `report.json`. Guest journal, dmesg, status,
configfs metrics, `fio` JSON, QEMU serial output, and command logs are retained
under the reported `results/vm/<run-id>/` directory.

Use `--keep-on-failure` to retain failed guests and their QMP sockets/disks for
inspection. A focused smoke run may lower resources within the selected cap and
set the soak to zero, but its report is marked non-certifiable:

```bash
tests/vm/run --kernel 6.8 --topology 2 --soak-hours 0 \
  --memory-gib 8 --disk-gib 24 --keep-on-failure
```

For repeatable completion-path performance evidence, select only
`fio-verification`. The harness expands that selection to build/deploy first and
the resource leak check last; it does not run the fault, reboot, reload, pressure,
or soak scenarios. The selection is always reported as non-certifiable, even
when it uses the full resource profile:

```bash
tests/vm/run --scenario fio-verification \
  --kernel 6.8 --topology 2 --soak-hours 0 \
  --artifacts results/vm/performance-baseline --json
```

Backed and Remote-Only each run one warmup and five measured 60-second verified
`fio` samples. Every raw sample is retained in an envelope with the source
commit, Ubuntu image checksum, exact kernel release, RDMA stack, topology, VM
resources, host identity, and effective `fio` arguments. Timeout or late RDMA
evidence, `fio` verification failure, outstanding I/O, kernel diagnostics, failed
cleanup, or a leak fails the run.

Compare a merge-base baseline and candidate with the machine-readable
comparison command:

```bash
tests/vm/compare \
  results/vm/performance-baseline/report.json \
  results/vm/performance-candidate/report.json \
  --output results/vm/performance-comparison.json
```

The comparison exits 2 when environments differ or baseline IOPS/throughput
coefficient of variation exceeds 5% or p99 latency coefficient of variation
exceeds 10%. It exits 1 for correctness evidence, an IOPS/throughput median
regression greater than 5%, a median p99 latency increase greater than 10%, or a
system-CPU increase greater than 10% reproduced in at least three samples.
Exact threshold values pass. Archive the complete baseline result directory,
not only `report.json`, because the report references the retained raw samples.
The first merge-base baseline is indexed at
`tests/vm/baselines/issue-18-soft-roce.json`. The default `tests/vm/run` plan
and certifiable report contract are unchanged.

Run `tests/vm/run --preflight-only --json` to validate a host without creating
or downloading anything. A release-gate report is certifiable only with the
full selected profile and at least 24 soak hours.

#### Persistent developer installation

`ty-gpu-02` has a persistent user installation for development and regression
work. `tests/vm/run` remains the canonical implementation; the installed
`infiniswap-vm` command is only a thin adapter that supplies managed cache and
result paths. Check it without starting a VM or changing host state:

```bash
ssh ty-gpu-02 'infiniswap-vm --preflight-only --json'
ssh ty-gpu-02 'git -C ~/.local/share/infiniswap-vm/source status -sb'
```

The installation lives under `~/.local/share/infiniswap-vm`: keep `cache/`
(Jammy and Noble base images), `archive/` (checksummed evidence), and `state/`
(installation and safety records). Completed entries under `results/` are
disposable after any important report has been archived. Installation details
and the reversible uninstall entry point are recorded in that directory's
`README.md` and `state/installation.json`.

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
