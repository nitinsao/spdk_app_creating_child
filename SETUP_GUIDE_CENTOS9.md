# SPDK/DPDK Shared Library Setup Guide — CentOS Stream 9

Complete steps to recreate the `mySPDKLib` + `mySPDKApp` project on CentOS Stream 9.

> This is the CentOS Stream 9 counterpart of `SETUP_GUIDE.md` (Ubuntu 24.04).
> Differences are: `dnf` package manager, RPM package names, EPEL repository for
> some tools, SELinux considerations, and a few build-tool version gaps.

---

## Overview

The project has three layers:

1. **SPDK/DPDK shared libraries** — built from source with `--with-shared`
2. **`mySPDKLib.so`** — a C++ shared library wrapping the SPDK NVMe driver
3. **`mySPDKApp`** — an application that issues raw NVMe commands, forks/execs a child, and demonstrates read/write across fork boundaries

---

## 1. Enable Required Repositories

CentOS Stream 9 ships a minimal set of development packages. Two extra repos are needed:

### 1a. EPEL (Extra Packages for Enterprise Linux)

Provides `nasm`, `meson`, `ninja-build`, `python3-pyelftools`, and others.

```bash
sudo dnf install -y epel-release
sudo dnf makecache
```

### 1b. CRB (CodeReady Builder / PowerTools)

Provides `CUnit-devel`, `libiscsi-devel`, and other `-devel` packages not in the
base repo.

```bash
sudo dnf config-manager --set-enabled crb
```

> On older CentOS 9 snapshots the repo may be called `powertools` — try both if
> `crb` is not recognised.

---

## 2. Install System Dependencies

```bash
sudo dnf update -y

sudo dnf install -y \
    gcc gcc-c++ make \
    git \
    cmake \
    python3 python3-pip \
    nasm \
    openssl-devel \
    libaio-devel \
    fuse3-devel \
    numactl-devel \
    CUnit-devel \
    libcmocka-devel \
    json-c-devel \
    keyutils-libs-devel \
    libiscsi-devel \
    libuuid-devel \
    autoconf automake libtool \
    patchelf \
    pkg-config \
    meson ninja-build \
    python3-pyelftools \
    rdma-core-devel libibverbs libibverbs-devel librdmacm-devel \
    kernel-modules-extra \
    python3-jsonschema \
    diffutils \
    file
```

> **Note on `nasm`**: EPEL for CentOS Stream 9 provides NASM 2.15.05 or later,
> which is sufficient for ISA-L assembly routines (minimum required is 2.14).
> Verify with `nasm --version` after installation.

> **Note on `meson`**: If the `meson` package from EPEL is older than 0.55.0
> (which DPDK 25.x requires), install a newer version via pip:
> ```bash
> pip3 install --user 'meson>=0.55.0'
> export PATH="$HOME/.local/bin:$PATH"
> ```

Alternatively, run SPDK's own dependency installer after cloning (step 3):

```bash
sudo scripts/pkgdep.sh              # auto-detects RHEL/CentOS
# or explicitly:
sudo scripts/pkgdep/rhel.sh
```

---

## 3. Install a Sufficiently New GCC (if needed)

CentOS Stream 9 ships **GCC 11** by default. SPDK and DPDK build correctly with GCC 11.
If you need GCC 13 (for C++23 features or to match the Ubuntu development environment),
install it via the gcc-toolset SCL:

```bash
sudo dnf install -y gcc-toolset-13
# Activate in your current shell:
source /opt/rh/gcc-toolset-13/enable
# Verify:
gcc --version   # should print 13.x
```

To activate permanently for a user, add the `source` line to `~/.bashrc`.

> If you only need to build this project, GCC 11 is fully sufficient — no SCL needed.

---

## 4. Clone SPDK with All Submodules

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

## 5. Configure SPDK for Shared Library Output

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
| `--without-isal` | Skip ISA-L if NASM is unavailable or too old |
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

## 6. Build SPDK and DPDK

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

## 7. Register Libraries with ldconfig

The application runs under `sudo` (required to access `/dev/uio*` and hugepage memory).
`sudo` strips `LD_LIBRARY_PATH`, so libraries must be registered system-wide.

```bash
# Replace /home/user/spdk with your actual SPDK clone path
sudo tee /etc/ld.so.conf.d/spdk.conf <<EOF
/home/user/spdk/build/lib
/home/user/spdk/dpdk/build/lib
EOF

sudo ldconfig
```

Verify:

```bash
ldconfig -p | grep libspdk_nvme
# Should print:
#   libspdk_nvme.so.18 (libc6,x86-64) => /home/user/spdk/build/lib/libspdk_nvme.so.18
```

---

## 8. Configure SELinux

CentOS Stream 9 ships with SELinux in **enforcing** mode by default.
The SPDK/DPDK user-space driver needs to:
- Access `/dev/uio*` character devices
- Map hugepage memory (`/dev/hugepages`)
- Read PCI sysfs entries (`/sys/bus/pci/...`)

### Option A: Permissive mode (quick — for development/lab use only)

```bash
sudo setenforce 0                    # temporary — reverts on reboot
# To make permanent:
sudo sed -i 's/^SELINUX=enforcing/SELINUX=permissive/' /etc/selinux/config
```

### Option B: Targeted policy (recommended for production)

Generate a custom policy module from audit denials after a first run in permissive mode:

```bash
# 1. Set permissive temporarily and run the app once to collect all denials:
sudo setenforce 0
sudo ./mySPDKApp/mySPDKApp   # generates AVC messages in /var/log/audit/audit.log

# 2. Convert the AVC messages to a policy module:
sudo ausearch -c 'mySPDKApp' --raw | audit2allow -M myspdk_policy

# 3. Install and activate the module:
sudo semodule -X 300 -i myspdk_policy.pp

# 4. Re-enable enforcing mode:
sudo setenforce 1
```

---

## 9. Set Up Hugepages

DPDK requires 2 MB hugepages for DMA-capable memory allocation.

```bash
# Allocate 512 × 2 MB = 1 GB of hugepages (adjust to your needs; 256 minimum)
sudo sh -c 'echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'

# Make the setting persist across reboots
echo 'vm.nr_hugepages = 512' | sudo tee /etc/sysctl.d/99-hugepages.conf
sudo sysctl -p /etc/sysctl.d/99-hugepages.conf
```

The SPDK `setup.sh` script handles this automatically (see next step).

---

## 10. Bind NVMe Devices to UIO (or VFIO)

> **Warning**: This unbinds the NVMe drive from the Linux kernel NVMe driver.
> Any filesystems on the device will become inaccessible until you re-bind.
> **Do not run this on your system/boot drive.**

### Option A: UIO (no IOMMU required — recommended for VMs and simple setups)

The `uio_pci_generic` module is part of the `kernel-modules-extra` package (already
installed in step 2).

```bash
# Load the UIO kernel modules
sudo modprobe uio
sudo modprobe uio_pci_generic

# Let SPDK's setup script allocate hugepages and bind all NVMe devices
sudo /path/to/spdk/scripts/setup.sh
```

To make the modules load at boot:

```bash
sudo tee /etc/modules-load.d/uio.conf <<EOF
uio
uio_pci_generic
EOF
```

Verify binding:

```bash
sudo /path/to/spdk/scripts/setup.sh status
# Expected output (example):
#   NVMe devices
#   BDF         Vendor Device NUMA    Driver           Device     NUMA
#   0000:00:0e.0 8086  5845  0       uio_pci_generic  -          -
#   hugepages
#   node0 2048kB: 512 / 512
```

### Option B: VFIO (requires IOMMU — recommended for production/security)

#### Enable IOMMU in the kernel boot parameters:

```bash
# For Intel CPUs:
sudo grubby --args="intel_iommu=on iommu=pt" --update-kernel=ALL

# For AMD CPUs:
sudo grubby --args="amd_iommu=on iommu=pt" --update-kernel=ALL

sudo reboot
```

After reboot, verify IOMMU is active:

```bash
dmesg | grep -i iommu | head -5
# Should show: "DMAR: IOMMU enabled" or similar
```

#### Bind devices with VFIO:

```bash
sudo modprobe vfio-pci
sudo /path/to/spdk/scripts/setup.sh
```

#### VFIO without IOMMU (for VMs where IOMMU passthrough is unavailable):

```bash
sudo modprobe vfio enable_unsafe_noiommu_mode=1
sudo modprobe vfio-pci
echo 1 | sudo tee /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
sudo /path/to/spdk/scripts/setup.sh
```

### Restoring the device to the kernel driver

```bash
sudo /path/to/spdk/scripts/setup.sh reset
```

---

## 11. Build the spdkApp Project

```bash
# From the directory where spdkApp/ lives (e.g., /home/user/spdk/)
cd /home/user/spdk/spdkApp
mkdir -p build && cd build

cmake .. \
    -DSPDK_ROOT_DIR=/home/user/spdk/spdk \
    -DCMAKE_BUILD_TYPE=Release

make -j$(nproc)
```

If using the gcc-toolset SCL (step 3), activate it before running cmake:

```bash
source /opt/rh/gcc-toolset-13/enable
cmake .. -DSPDK_ROOT_DIR=/home/user/spdk/spdk -DCMAKE_BUILD_TYPE=Release
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

## 12. Run the Application

```bash
cd /home/user/spdk/spdkApp/build
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

## 13. Project Directory Structure

```
spdk/                              ← SPDK source + build (Layer 1)
│   build/lib/libspdk_*.so         ← SPDK shared libraries
│   dpdk/build/lib/librte_*.so     ← DPDK shared libraries
│
└── spdkApp/                       ← CMake project root
    ├── CMakeLists.txt             ← Root CMake; sets SPDK/DPDK paths
    ├── README.md                  ← Detailed architecture + API docs
    ├── SETUP_GUIDE.md             ← Ubuntu 24.04 setup guide
    ├── SETUP_GUIDE_CENTOS9.md     ← This file
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

## 14. CMake Variables Reference

| Variable | Default | Description |
|----------|---------|-------------|
| `SPDK_ROOT_DIR` | `${PROJECT_SOURCE_DIR}/../spdk` | Path to the SPDK source+build tree |
| `SPDK_LIB_DIR` | `${SPDK_ROOT_DIR}/build/lib` | Where `libspdk_*.so` files live |
| `SPDK_INC_DIR` | `${SPDK_ROOT_DIR}/include` | SPDK public headers |
| `DPDK_LIB_DIR` | `${SPDK_ROOT_DIR}/dpdk/build/lib` | Where `librte_*.so` files live |

Override any of these at cmake time with `-DVARIABLE=value`.

---

## 15. Troubleshooting

### `libspdk_log.so.9.0: cannot open shared object file`

`sudo` strips `LD_LIBRARY_PATH`. Fix: ensure `/etc/ld.so.conf.d/spdk.conf` has the
correct paths and `sudo ldconfig` was run (step 7).

```bash
ldconfig -p | grep libspdk_log
```

### `spdk_vtophys() failed` / `0 controller(s) found after reinit`

The `reinit()` function must call `spdk_env_dpdk_post_init(false)` (from
`<spdk/env_dpdk.h>`), **not** `spdk_env_init(NULL)`. The latter only runs
`pci_env_reinit()` and skips `vtophys_init()` / `mem_map_init()`, breaking all
DMA address translations.

### Hugepages not allocated

```bash
grep HugePages_Total /proc/meminfo
# Must be > 0

echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

### `uio_pci_generic: No such file or directory`

The module is in `kernel-modules-extra`, which must be installed:

```bash
sudo dnf install -y kernel-modules-extra
sudo modprobe uio_pci_generic
```

### Device not bound to UIO

```bash
sudo /path/to/spdk/scripts/setup.sh status
# If the NVMe device shows "nvme" as driver, re-run:
sudo /path/to/spdk/scripts/setup.sh
```

### Build fails: `meson` version too old

DPDK 25.x requires Meson ≥ 0.55.0. The EPEL package may be older on some
CentOS 9 snapshots. Install a newer version via pip:

```bash
pip3 install --user 'meson>=0.55.0'
export PATH="$HOME/.local/bin:$PATH"
meson --version
```

### Build fails: `nasm` not found or version too old

```bash
sudo dnf install -y nasm       # from EPEL
nasm --version                 # must be >= 2.14
```

If EPEL's NASM is still too old, build from source:

```bash
curl -LO https://www.nasm.us/pub/nasm/releasebuilds/2.16.01/nasm-2.16.01.tar.gz
tar xf nasm-2.16.01.tar.gz && cd nasm-2.16.01
./configure && make -j$(nproc) && sudo make install
```

Or skip ISA-L entirely:

```bash
./configure --with-shared --without-isal
```

### Build fails: `CUnit/CUnit.h: No such file or directory`

CRB repo must be enabled:

```bash
sudo dnf config-manager --set-enabled crb
sudo dnf install -y CUnit-devel
```

### Application crashes with `avc: denied` in `/var/log/audit/audit.log`

SELinux is blocking access. Either set permissive mode (step 8, Option A) or
generate a targeted policy module (step 8, Option B).

Quick check:

```bash
sudo ausearch -m AVC -ts recent | grep mySPDKApp
```

### Application exits immediately with "spdk_env_init() failed"

- Hugepages not allocated — see above.
- Insufficient permissions — run with `sudo`.
- SELinux blocking hugepage access — check AVC log.
- Another process holds the hugepage lock:
  ```bash
  sudo pkill -f mySPDKApp
  sudo pkill -f spdk
  ```

---

## 16. Ubuntu vs CentOS Stream 9 — Package Name Mapping

| Purpose | Ubuntu 24.04 | CentOS Stream 9 |
|---------|-------------|-----------------|
| C++ compiler | `gcc g++` | `gcc gcc-c++` |
| OpenSSL headers | `libssl-dev` | `openssl-devel` |
| AIO support | `libaio-dev` | `libaio-devel` |
| FUSE3 headers | `libfuse3-dev` | `fuse3-devel` |
| NUMA headers | `libnuma-dev` | `numactl-devel` |
| CUnit testing | `libcunit1-dev` | `CUnit-devel` (CRB) |
| cmocka | `libcmocka-dev` | `libcmocka-devel` |
| JSON-C | `libjson-c-dev` | `json-c-devel` |
| Keyutils | `libkeyutils-dev` | `keyutils-libs-devel` |
| iSCSI | `libiscsi-dev` | `libiscsi-devel` (CRB) |
| UUID | `uuid-dev` | `libuuid-devel` |
| PyELF | `python3-pyelftools` | `python3-pyelftools` (EPEL) |
| RDMA | `libibverbs-dev librdmacm-dev` | `libibverbs-devel librdmacm-devel` |
| NASM | `nasm` | `nasm` (EPEL) |
| Meson | `meson` | `meson` (EPEL) or `pip3 install meson` |
| Ninja | `ninja-build` | `ninja-build` (EPEL) |
| Patchelf | `patchelf` | `patchelf` (EPEL) |
| UIO module | built-in | `kernel-modules-extra` |
| SELinux tools | n/a (AppArmor) | `audit2allow` (`policycoreutils-python-utils`) |

---

## 17. Version Reference

Versions used during development (Ubuntu); CentOS 9 counterparts listed where different:

| Component | Ubuntu 24.04 | CentOS Stream 9 |
|-----------|-------------|-----------------|
| GCC | 13.3.0 | 11.x (default) or 13.x via gcc-toolset-13 |
| CMake | 3.28.3 | 3.26.x |
| NASM | 2.16.01 | 2.15.05 (EPEL) |
| OpenSSL | 3.x | 3.x |
| SPDK | v26.05-pre | same |
| DPDK | 25.11.0 | same |
| ISA-L | v2.31.0 | same |
| ISA-L-crypto | v2.24.0 | same |

---

## 18. Quick-Start Checklist

- [ ] Enable EPEL: `sudo dnf install -y epel-release`
- [ ] Enable CRB: `sudo dnf config-manager --set-enabled crb`
- [ ] Install packages (`dnf install` list from step 2, or `scripts/pkgdep/rhel.sh`)
- [ ] (Optional) Install gcc-toolset-13 and activate it
- [ ] Clone SPDK with submodules (`git clone --recurse-submodules`)
- [ ] `./configure --with-shared`
- [ ] `make -j$(nproc)` — verify `.so` files appear in `build/lib/` and `dpdk/build/lib/`
- [ ] Create `/etc/ld.so.conf.d/spdk.conf` + `sudo ldconfig`
- [ ] Configure SELinux (permissive or custom policy module)
- [ ] `sudo modprobe uio uio_pci_generic`
- [ ] `sudo scripts/setup.sh` — allocates hugepages + binds NVMe to UIO
- [ ] `cmake .. && make -j$(nproc)` in `spdkApp/build/`
- [ ] `sudo ./mySPDKApp/mySPDKApp`
