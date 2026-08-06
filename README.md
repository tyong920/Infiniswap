# Infiniswap

Infiniswap lets a Memory Consumer use host DRAM contributed by one or more
Memory Providers over RDMA. The project preserves the original one-sided RDMA
architecture while modernizing it for controlled production use.

The current supported build targets are:

- Ubuntu 22.04 with the Linux 5.15 GA kernel, including Linux 5.15 with
  MLNX_OFED 5.8.
- Ubuntu 24.04 with the Linux 6.8 GA kernel and inbox RDMA.

The modernization is incremental. The current Memory Consumer milestone exposes
a Backed Mode Infiniswap Device that routes all I/O to a dedicated Backing Store;
its Remote Memory data path is disabled until the later Backed Mode RDMA
milestone. It is suitable for the privileged block verification described here,
not production swap activation. Runtime validation and production operations
are tracked separately in `docs/modernization-plan.md`.

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

Pass these cache variables to the CMake configure command with `-D`:

| Variable | Default | Meaning |
| --- | ---: | --- |
| `INFINISWAP_MAX_CLIENT` | `32` | Maximum connected Memory Consumers |
| `INFINISWAP_MAX_REMOTE_MEMORY_GB` | `32` | Maximum contributed Remote Memory |
| `INFINISWAP_REMOTE_MEMORY_EVICT_GB` | `8` | Eviction threshold |
| `INFINISWAP_EVICT_HIT_LIMIT` | `1` | Low-memory samples before eviction |
| `INFINISWAP_REMOTE_MEMORY_EXPAND_GB` | `16` | Expansion threshold |
| `INFINISWAP_EXPAND_HIT_LIMIT` | `20` | High-memory samples before expansion |
| `INFINISWAP_MEASURED_FREE_MEM_WEIGHT` | `0.7` | Current free-memory sample weight |

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

### Backed Mode Device

The module uses configfs as its lifecycle seam. Device creation, activation,
swap formatting, and swap activation are separate operations. The module never
runs `mkswap`, `swapon`, or `swapoff`.

Load the module, create an inactive configuration, and activate it only after
selecting a dedicated block device, partition, or LVM logical volume:

```bash
sudo modprobe configfs
mountpoint -q /sys/kernel/config || \
  sudo mount -t configfs none /sys/kernel/config
sudo insmod infiniswap_bd/infiniswap.ko

sudo mkdir /sys/kernel/config/infiniswap/infiniswap0
echo backed | \
  sudo tee /sys/kernel/config/infiniswap/infiniswap0/mode
echo /dev/vdb | \
  sudo tee /sys/kernel/config/infiniswap/infiniswap0/backing_store
echo $((64 * 1024 * 1024 * 1024)) | \
  sudo tee /sys/kernel/config/infiniswap/infiniswap0/capacity_bytes
echo activate | \
  sudo tee /sys/kernel/config/infiniswap/infiniswap0/state
cat /sys/kernel/config/infiniswap/infiniswap0/state
```

`mode` explicitly selects Backed Mode and cannot change while the device is
active. `backing_store` rejects regular files, loop devices, read-only devices,
and Infiniswap devices. Activation opens the Backing Store exclusively and
rejects capacities larger than it or misaligned to its logical block size before
`/dev/infiniswap0` is exposed. The device mirrors the Backing Store's write-cache
and FUA capabilities and forwards flushes. Discard and write-zeroes are not
advertised and are rejected if sent.

Close every user and disable this specific device as swap, if an administrator
enabled it separately, before draining and stopping it:

```bash
echo drain | \
  sudo tee /sys/kernel/config/infiniswap/infiniswap0/state
echo stop | \
  sudo tee /sys/kernel/config/infiniswap/infiniswap0/state
sudo rmdir /sys/kernel/config/infiniswap/infiniswap0
sudo modprobe -r infiniswap
```

`drain` removes the block device from userspace and waits for every accepted
request to complete. It returns `EBUSY` while the device has open users. `stop`
releases its queue, minor, and exclusive Backing Store holder. An active or
drained configfs item is pinned until `stop` succeeds, so it cannot be destroyed
out from under in-flight I/O.

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

`setup/install.sh bd` only builds and installs the module. It does not load the
module, create an Infiniswap Device, format swap, or alter active swap.

## Continuous Integration

`.github/workflows/build.yml` runs the Linux 5.15 and 6.8 module compile matrix,
builds the Memory Provider with warnings as errors on Ubuntu 22.04 and 24.04,
and runs the daemon tests.

## Research Background

The original Infiniswap design and evaluation appeared at NSDI 2017:
[Infiniswap: Efficient Memory Disaggregation](https://www.usenix.org/conference/nsdi17/technical-sessions/presentation/gu).
