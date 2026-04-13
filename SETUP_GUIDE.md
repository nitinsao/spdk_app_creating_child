# SPDK/DPDK Shared Library Setup Guide

Complete steps to recreate the `mySPDKLib` + `mySPDKApp` project on a fresh machine.

Tested on: **Ubuntu 24.04.3 LTS**, kernel 6.17.0, GCC 13.3.0, CMake 3.28.3

---

## Overview

The project has three layers:

1. **SPDK/DPDK shared libraries** — built from source with `--with-shared`
2. **`mySPDKLib.so`** — a C++ shared library wrapping the SPDK NVMe driver
3. **`mySPDKApp`** — an application that issues raw NVMe commands, forks/execs a child, and demonstrates read/write across fork boundaries

---

## 1. Install System Dependencies

```bash
sudo apt-get update
sudo apt-get install -y \
    gcc g++ make \
    git \
    cmake \
    python3 python3-pip \
    nasm \
    libssl-dev \
    libaio-dev \
    libfuse3-dev \
    libnuma-dev \
    libcunit1-dev \
    libcmocka-dev \
    libjson-c-dev \
    libkeyutils-dev \
    libiscsi-dev \
    uuid-dev \
    autoconf automake libtool \
    patchelf \
    pkg-config \
    meson ninja-build \
    python3-pyelftools \
    rdma-core libibverbs-dev librdmacm-dev
```

> **NASM note**: NASM 2.14 or later is required for ISA-L assembly routines.
> The `nasm` package in Ubuntu 24.04 provides 2.16.01 which is sufficient.

Alternatively, run SPDK's own dependency installer after cloning (step 2):

```bash
sudo scripts/pkgdep.sh          # detects OS automatically
# or explicitly:
sudo scripts/pkgdep/ubuntu.sh
```

---

## 2. Clone SPDK with All Submodules

```bash
git clone --recurse-submodules https://github.com/spdk/spdk.git
cd spdk
```

This pulls in:
- `dpdk/` — DPDK 25.11.x (embedded submodule, ~100 MB)
- `isa-l/` — Intel ISA-L v2.31.x (compression acceleration)
- `isa-l_crypto/` — Intel ISA-L-crypto v2.24.x (crypto acceleration)

If you cloned without `--recurse-submodules`, initialise them now:

```bash
git submodule update --init --recursive
```

---

## 3. Configure SPDK for Shared Library Output

```bash
./configure --with-shared
```

Key effects of `--with-shared`:
- Sets `CONFIG_SHARED=y` in `mk/config.mk`
- Each library's Makefile builds **both** `lib<name>.a` (static) and `lib<name>.so` (shared)
- DPDK is also built with `-DBUILD_SHARED_LIBS=ON` via its embedded Meson build

Optional flags you may want to add:

| Flag | Purpose |
|------|---------|
| `--without-isal` | Skip ISA-L if NASM is unavailable |
| `--with-rdma` | Enable NVMe-oF RDMA transport |
| `--disable-tests` | Skip building unit tests (faster) |
| `--prefix=/opt/spdk` | Install to a custom prefix |

Verify the configuration was applied:

```bash
grep 'CONFIG_SHARED\|configure options' mk/config.mk
# Expected:
#   # configure options: --with-shared
#   CONFIG_SHARED=y
```

---

## 4. Build SPDK and DPDK

```bash
make -j$(nproc)
```

This builds:
- DPDK: `dpdk/build/lib/librte_*.so` (≈ 30 shared libs)
- SPDK: `build/lib/libspdk_*.so` (≈ 77 shared libs)
- All matching static `.a` archives in the same directories

Expected output locations:

```
spdk/
├── build/
│   └── lib/
│       ├── libspdk_nvme.so.18.0       ← NVMe driver
│       ├── libspdk_env_dpdk.so.17.0   ← DPDK environment adapter
│       ├── libspdk_log.so.9.0
│       └── ...  (74 more)
└── dpdk/
    └── build/
        └── lib/
            ├── librte_eal.so.25.1      ← DPDK EAL
            ├── librte_mempool.so.6.1
            └── ...  (28 more)
```

Verify the shared libraries were created:

```bash
ls build/lib/libspdk_nvme.so*
ls dpdk/build/lib/librte_eal.so*
```

---

## 5. Register Libraries with ldconfig

The application runs under `sudo` (required to access `/dev/uio*` and hugepage memory).
`sudo` strips `LD_LIBRARY_PATH`, so libraries must be registered system-wide.

```bash
# Replace /home/nitin/spdk/spdk with your actual SPDK clone path
sudo tee /etc/ld.so.conf.d/spdk.conf <<EOF
/home/nitin/spdk/spdk/build/lib
/home/nitin/spdk/spdk/dpdk/build/lib
EOF

sudo ldconfig
```

Verify:

```bash
ldconfig -p | grep libspdk_nvme
# Should print:
#   libspdk_nvme.so.18 (libc6,x86-64) => /home/nitin/spdk/spdk/build/lib/libspdk_nvme.so.18
```

---

## 6. Set Up Hugepages

DPDK requires 2 MB hugepages for DMA-capable memory allocation.

```bash
# Allocate 512 × 2 MB = 1 GB of hugepages (adjust to your needs; 256 minimum)
sudo sh -c 'echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'

# Make the setting persist across reboots
echo 'vm.nr_hugepages = 512' | sudo tee /etc/sysctl.d/99-hugepages.conf
```

The SPDK `setup.sh` script handles this automatically (see next step).

---

## 7. Bind NVMe Devices to UIO (or VFIO)

> **Warning**: This unbinds the NVMe drive from the Linux kernel NVMe driver.
> Any filesystems on the device will become inaccessible until you re-bind.
> **Do not run this on your system/boot drive.**

### Option A: UIO (no IOMMU required — recommended for VMs and simple setups)

```bash
# Load the UIO kernel modules
sudo modprobe uio
sudo modprobe uio_pci_generic

# Let SPDK's setup script allocate hugepages and bind all NVMe devices
sudo /path/to/spdk/scripts/setup.sh
```

Default behaviour: allocates hugepages (or uses existing), binds all NVMe PCIe devices to `uio_pci_generic`.

Verify binding:

```bash
sudo /path/to/spdk/scripts/setup.sh status
# Expected output (example):
#   NVMe devices
#   BDF         Vendor Device NUMA    Driver           Device     NUMA
#   0000:00:0e.0 8086  5845  0       uio_pci_generic  -          -
#   hugepages
#   node0 2048kB: 218 / 218
```

### Option B: VFIO (requires IOMMU — recommended for production/security)

```bash
# Enable IOMMU in GRUB: add intel_iommu=on (or amd_iommu=on) to GRUB_CMDLINE_LINUX
# Then:
sudo modprobe vfio-pci
sudo /path/to/spdk/scripts/setup.sh
```

### Restoring the device to the kernel driver

```bash
sudo /path/to/spdk/scripts/setup.sh reset
```

---

## 8. Build the spdkApp Project

```bash
# From the repo root (where spdkApp/ lives)
cd /home/nitin/spdk/spdkApp
mkdir -p build && cd build

cmake .. \
    -DSPDK_ROOT_DIR=/home/nitin/spdk/spdk \
    -DCMAKE_BUILD_TYPE=Release

make -j$(nproc)
```

If SPDK is at the default location (`../spdk/` relative to `spdkApp/`), you can omit `-DSPDK_ROOT_DIR`.

Build outputs:

```
spdkApp/build/
├── mySPDKLib/
│   ├── libmySPDKLib.so.1.0.0   ← shared library
│   └── libmySPDKLib.so.1       ← soname symlink
└── mySPDKApp/
    └── mySPDKApp               ← executable
```

---

## 9. Run the Application

```bash
cd /home/nitin/spdk/spdkApp/build
sudo ./mySPDKApp/mySPDKApp
```

Expected output (abbreviated):

```
[mySPDKLib] Probe: found controller at 0000:00:0e.0 — attaching
[mySPDKLib] Attached controller: BDF=0000:00:0e.0
[mySPDKLib] init() complete — 1 controller(s) found

=== Controller 0: BDF=0000:00:0e.0 ===
  Model   : ORCL-VBOX-NVME-VER12
  Firmware: 1.0
  Serial  : VB1234-56789

=== Namespace 1 ===
  Size       : 536870912 sectors
  Sector size: 512 B

[Parent] Reading SLBA 0 before fork ...
[Parent] First 20 bytes at SLBA 0: ...

[mySPDKLib] deinit() complete

[Child] init() complete
[Child] Writing "HelloWorld123456789" to SLBA 0 ...
[Child] Write complete (status=0)
[Child] Reading back 25 bytes from SLBA 0 ...
[Child] Read data: HelloWorld123456789

[mySPDKLib] reinit() complete — 1 controller(s) found
[Parent] Reading SLBA 0 after child write ...
[Parent] First 20 bytes at SLBA 0: HelloWorld123456789
```

---

## 10. Project Directory Structure

```
spdk/                              ← SPDK source + build (Layer 1)
│   build/lib/libspdk_*.so         ← SPDK shared libraries
│   dpdk/build/lib/librte_*.so     ← DPDK shared libraries
│
└── spdkApp/                       ← CMake project root
    ├── CMakeLists.txt             ← Root CMake; sets SPDK/DPDK paths
    ├── README.md                  ← Detailed architecture + API docs
    ├── SETUP_GUIDE.md             ← This file
    │
    ├── mySPDKLib/                 ← Layer 2: shared library
    │   ├── CMakeLists.txt
    │   ├── mySPDKLib.h            ← Public C-linkage API
    │   └── mySPDKLib.cpp          ← Implementation
    │
    └── mySPDKApp/                 ← Layer 3: application
        ├── CMakeLists.txt
        └── mySPDKApp.cpp
```

---

## 11. CMake Variables Reference

| Variable | Default | Description |
|----------|---------|-------------|
| `SPDK_ROOT_DIR` | `${PROJECT_SOURCE_DIR}/../spdk` | Path to the SPDK source+build tree |
| `SPDK_LIB_DIR` | `${SPDK_ROOT_DIR}/build/lib` | Where `libspdk_*.so` files live |
| `SPDK_INC_DIR` | `${SPDK_ROOT_DIR}/include` | SPDK public headers |
| `DPDK_LIB_DIR` | `${SPDK_ROOT_DIR}/dpdk/build/lib` | Where `librte_*.so` files live |

Override any of these at cmake time with `-DVARIABLE=value`.

---

## 12. Troubleshooting

### `libspdk_log.so.9.0: cannot open shared object file`

The dynamic linker cannot find a transitive SPDK dependency. This typically happens under `sudo` because `LD_LIBRARY_PATH` is stripped.

Fix: ensure `/etc/ld.so.conf.d/spdk.conf` exists with the correct paths and `sudo ldconfig` was run (see step 5).

```bash
# Verify the library is in the ldconfig cache
ldconfig -p | grep libspdk_log
```

### `spdk_vtophys() failed` / `0 controller(s) found after reinit`

This indicates that `reinit()` restored the SPDK environment incorrectly.
The correct call after `deinit()` in the **same process** (where the DPDK EAL is still alive) is:

```cpp
spdk_env_dpdk_post_init(/*legacy_mem=*/false);   // from <spdk/env_dpdk.h>
```

**Do NOT** call `spdk_env_init(NULL)` for reinit — that path only calls
`pci_env_reinit()` and skips `vtophys_init()` / `mem_map_init()`, leaving all
DMA address translations broken.

### Hugepages not allocated

```bash
cat /proc/meminfo | grep -i huge
# HugePages_Total must be > 0

# If zero, allocate manually:
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

### Device not bound to UIO

```bash
sudo /path/to/spdk/scripts/setup.sh status
# If the NVMe device shows "nvme" in the Driver column, re-run:
sudo /path/to/spdk/scripts/setup.sh
```

### Build fails: `meson` or `ninja` not found

```bash
sudo apt-get install -y meson ninja-build python3-pyelftools
```

### Build fails: `nasm` version too old

NASM < 2.14 cannot assemble ISA-L routines. Check:

```bash
nasm --version
```

Install a newer version from the NASM official package or build from source if the distro package is too old. Alternatively, configure SPDK without ISA-L:

```bash
./configure --with-shared --without-isal
```

### Application exits immediately with "spdk_env_init() failed"

- Hugepages not allocated — see above.
- Insufficient permissions — run with `sudo`.
- Another process holds the hugepage lock — check for stale SPDK/DPDK processes:
  ```bash
  sudo pkill -f mySPDKApp
  sudo pkill -f spdk
  ```

---

## 13. Version Reference

Versions used during development of this project:

| Component | Version |
|-----------|---------|
| OS | Ubuntu 24.04.3 LTS |
| Kernel | 6.17.0-14-generic |
| GCC | 13.3.0 |
| CMake | 3.28.3 |
| NASM | 2.16.01 |
| SPDK | v26.05-pre (commit `e0dc3f70a`) |
| DPDK | 25.11.0 (SPDK embedded submodule) |
| ISA-L | v2.31.0 |
| ISA-L-crypto | v2.24.0 |
| OpenSSL | 3.x (system) |

---

## 14. Quick-Start Checklist

- [ ] Install packages (`apt-get install` or `scripts/pkgdep.sh`)
- [ ] Clone SPDK with submodules (`git clone --recurse-submodules`)
- [ ] `./configure --with-shared`
- [ ] `make -j$(nproc)` — verify `.so` files appear in `build/lib/` and `dpdk/build/lib/`
- [ ] Create `/etc/ld.so.conf.d/spdk.conf` + `sudo ldconfig`
- [ ] Load `uio` and `uio_pci_generic` kernel modules
- [ ] `sudo scripts/setup.sh` — allocates hugepages + binds NVMe to UIO
- [ ] `cmake .. && make -j$(nproc)` in `spdkApp/build/`
- [ ] `sudo ./mySPDKApp/mySPDKApp`
