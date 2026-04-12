# spdkApp — 3-Layer SPDK/DPDK NVMe Exercise Project

A three-layer C++ project that demonstrates raw NVMe admin and I/O command
execution using the SPDK (Storage Performance Development Kit) user-space
driver, with a fork/exec child-process pattern for multi-process device
access.

---

## Table of Contents

1. [Project Architecture](#1-project-architecture)
2. [Directory Structure](#2-directory-structure)
3. [Layer Details](#3-layer-details)
   - [Layer 1 — SPDK/DPDK Shared Libraries](#layer-1--spdkdpdk-shared-libraries)
   - [Layer 2 — mySPDKLib](#layer-2--myspdklib)
   - [Layer 3 — mySPDKApp](#layer-3--myspdkapp)
4. [API Reference (mySPDKLib)](#4-api-reference-myspdklib)
5. [Prerequisites](#5-prerequisites)
6. [Build Instructions](#6-build-instructions)
7. [Running the Application](#7-running-the-application)
8. [Detailed Execution Flow](#8-detailed-execution-flow)
9. [NVMe Commands Used](#9-nvme-commands-used)
10. [Key Design Decisions](#10-key-design-decisions)
11. [Troubleshooting](#11-troubleshooting)

---

## 1. Project Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  Layer 3  │  mySPDKApp  (executable)                            │
│           │  Identifies controllers & namespaces, forks child,  │
│           │  reads/writes NVMe sectors via raw commands          │
├───────────┼─────────────────────────────────────────────────────┤
│  Layer 2  │  libmySPDKLib.so  (shared library)                  │
│           │  init / deinit / reinit / execute_admin_commands /   │
│           │  execute_io_commands — C-linkage API                 │
├───────────┼─────────────────────────────────────────────────────┤
│  Layer 1  │  libspdk_*.so  +  librte_*.so                       │
│           │  SPDK NVMe user-space driver + DPDK EAL             │
│           │  (built from /home/nitin/spdk/spdk with --with-shared)│
└───────────┴─────────────────────────────────────────────────────┘
```

Each layer is a separate build unit. Layers 2 and 3 are compiled together
under a single CMake project (`spdkApp/`). Layer 1 is built once from the
SPDK source tree; its `.so` files are consumed dynamically at runtime.

---

## 2. Directory Structure

```
spdk/
├── spdk/                          ← SPDK source + build tree (Layer 1)
│   ├── build/lib/                 ← libspdk_*.so  (76 shared libraries)
│   ├── dpdk/build/lib/            ← librte_*.so   (30 shared libraries)
│   └── include/spdk/              ← Public SPDK headers
│
└── spdkApp/                       ← CMake project (Layers 2 & 3)
    ├── CMakeLists.txt             ← Root CMake — discovers SPDK paths
    ├── README.md                  ← This file
    │
    ├── mySPDKLib/                 ← Layer 2 sub-project
    │   ├── CMakeLists.txt
    │   ├── mySPDKLib.h            ← Public C-linkage API
    │   └── mySPDKLib.cpp          ← Implementation
    │
    ├── mySPDKApp/                 ← Layer 3 sub-project
    │   ├── CMakeLists.txt
    │   └── mySPDKApp.cpp          ← Application entry point
    │
    └── build/                     ← CMake out-of-source build directory
        ├── mySPDKLib/
        │   └── libmySPDKLib.so    ← Built shared library (Layer 2)
        └── mySPDKApp/
            └── mySPDKApp          ← Built executable  (Layer 3)
```

---

## 3. Layer Details

### Layer 1 — SPDK/DPDK Shared Libraries

SPDK provides a user-space NVMe driver built on top of DPDK's EAL (Environment
Abstraction Layer). It bypasses the kernel storage stack, allowing the
application to own the device directly via VFIO or UIO.

**Build status:** Already compiled with `CONFIG_SHARED=y`. If a rebuild is
needed:

```bash
cd /home/nitin/spdk/spdk
./configure --with-shared
make -j$(nproc)
```

Key libraries consumed by mySPDKLib:

| Library | Purpose |
|---|---|
| `libspdk_nvme.so` | NVMe PCIe initiator driver |
| `libspdk_env_dpdk.so` | DPDK EAL wrapper |
| `libspdk_log.so` | Logging subsystem |
| `libspdk_util.so` | Utility functions |
| `libspdk_trace.so` | Tracing subsystem |
| `libspdk_dma.so` | DMA buffer helpers |
| `librte_eal.so` | DPDK EAL (hugepages, CPU pinning) |
| `librte_mempool.so` | DPDK memory pool allocator |
| `librte_bus_pci.so` | DPDK PCIe bus driver |

---

### Layer 2 — mySPDKLib

**File:** `mySPDKLib/mySPDKLib.cpp`  
**Output:** `build/mySPDKLib/libmySPDKLib.so.1.0.0`

A C++ shared library that wraps the asynchronous SPDK NVMe driver behind a
simple **synchronous** C-linkage API.  All callers pass ordinary heap pointers
as data buffers; the library handles DMA allocation, data copy, and completion
polling internally.

**Internal design highlights:**

- **Probe callbacks** — `probe_cb` and `attach_cb` are registered with
  `spdk_nvme_probe()`.  Every discovered PCIe NVMe controller is stored in a
  process-local `std::vector<ControllerEntry>` indexed by a 0-based integer.

- **Synchronous wrapper** — A `CmdContext{ volatile bool done; int status; }`
  struct is passed as the callback argument.  After submission the library
  spins on `done` while calling the appropriate SPDK poll function
  (`spdk_nvme_ctrlr_process_admin_completions` or
  `spdk_nvme_qpair_process_completions`), converting async completion into a
  blocking call.

- **DMA buffer management** — For each command the library allocates a
  temporary DMA-capable buffer via `spdk_zmalloc(len, 4096, NULL,
  SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA)`, copies the caller's data in
  (for writes / outbound commands), submits the command, and copies results
  back (for reads / inbound commands) before freeing the buffer with
  `spdk_free()`.

- **Per-call I/O queue pairs** — `execute_io_commands()` allocates an I/O
  queue pair with `spdk_nvme_ctrlr_alloc_io_qpair()` and frees it with
  `spdk_nvme_ctrlr_free_io_qpair()` around each command.  This is simpler
  and fork-safe at the cost of some latency.

- **Fork safety** — `deinit()` calls `spdk_nvme_detach_async()` for every
  controller followed by `spdk_env_fini()` (which runs
  `spdk_env_dpdk_post_fini()`: destroys vtophys tables, memory map, and PCI
  layer).  The DPDK EAL (hugepages, memory segments) stays alive — only
  `rte_eal_cleanup()`, called by a process destructor at exit, fully tears it
  down.  `reinit()` therefore calls `spdk_env_dpdk_post_init(false)` (the
  public API) which re-runs all three post-init steps (PCI, mem-map, vtophys)
  on top of the still-live EAL — restoring full DMA capability without
  needing a second `rte_eal_init()`.

---

### Layer 3 — mySPDKApp

**File:** `mySPDKApp/mySPDKApp.cpp`  
**Output:** `build/mySPDKApp/mySPDKApp`

The application runs in two modes selected by argv[1]:

| Mode | Invocation | Description |
|---|---|---|
| **Parent** | `mySPDKApp` | Full flow: identify → fork → read |
| **Child** | `mySPDKApp child` | Write + read at SLBA 0 |

---

## 4. API Reference (mySPDKLib)

All functions use C linkage (`extern "C"`).

```c
/* Initialise SPDK/DPDK and probe all PCIe NVMe controllers.
 * Idempotent — a second call while already initialised returns 0.
 * Returns 0 on success, negative errno on failure. */
int init(void);

/* Detach all controllers and run spdk_env_dpdk_post_fini().
 * Call before fork() when the child process needs independent device access.
 * The DPDK EAL hugepages remain alive; reinit() re-uses them. */
void deinit(void);

/* Re-run spdk_env_dpdk_post_init() and re-probe all PCIe NVMe controllers.
 * Must be called after deinit() within the same process.
 * Returns 0 on success, negative errno on failure. */
int reinit(void);

/* Number of currently attached controllers. */
int get_num_controllers(void);

/* Opaque controller handle (0-based index). NULL if out of range. */
struct spdk_nvme_ctrlr *get_controller(int idx);

/* Transport ID for a controller — contains the PCIe BDF address. */
const struct spdk_nvme_transport_id *get_controller_trid(int idx);

/* Total namespace count for a controller (1-based NSID range). */
uint32_t get_num_namespaces(int ctrlr_idx);

/* Formatted LBA size (sector size in bytes) for nsid on ctrlr_idx. */
uint32_t get_sector_size(int ctrlr_idx, uint32_t nsid);

/* Submit a raw NVMe Admin command and block until completion.
 *   ctrlr_idx — 0-based controller index
 *   cmd       — caller fills opc, nsid, cdw10-cdw15; DPTR is overwritten
 *   buf       — caller heap buffer; copied to/from internal DMA region
 *   len       — size of buf in bytes
 * Returns 0 on success, negative on submission error, 1 on NVMe error. */
int execute_admin_commands(int ctrlr_idx, struct spdk_nvme_cmd *cmd,
                           void *buf, uint32_t len);

/* Submit a raw NVMe I/O command and block until completion.
 *   cmd->opc   — NVMe I/O opcode (0x01 Write, 0x02 Read, …)
 *   cmd->nsid  — 1-based namespace ID
 *   cmd->cdw10 — SLBA bits [31:0]
 *   cmd->cdw11 — SLBA bits [63:32]
 *   cmd->cdw12 — Number of Logical Blocks − 1  (NLB field)
 *   buf / len  — sector-aligned caller buffer
 * Returns 0 on success, negative on submission error, 1 on NVMe error. */
int execute_io_commands(int ctrlr_idx, struct spdk_nvme_cmd *cmd,
                        void *buf, uint32_t len);
```

---

## 5. Prerequisites

### Hardware / VM

- An NVMe device (physical or emulated) bound to `uio_pci_generic` or
  `vfio-pci`.  In the test environment this is a VirtualBox NVMe device at
  BDF `0000:00:0e.0`.

### Hugepages

2 MB hugepages must be pre-allocated.  Use SPDK's setup script:

```bash
# Allocate 512 MB of hugepages and bind NVMe to UIO
sudo /home/nitin/spdk/spdk/scripts/setup.sh

# Verify
sudo /home/nitin/spdk/spdk/scripts/setup.sh status
# Expected output:
# node0   2048kB   218 / 218
# NVMe    0000:00:0e.0   ...   uio_pci_generic
```

To release hugepages and restore the kernel NVMe driver after testing:

```bash
sudo /home/nitin/spdk/spdk/scripts/setup.sh reset
```

### System Packages

```bash
# Ubuntu / Debian
sudo apt install libfuse3-dev libnuma-dev libuuid1 libssl-dev \
                 cmake g++ pkg-config

# The ldconfig cache must include SPDK and DPDK lib paths (done once):
echo -e "/home/nitin/spdk/spdk/build/lib\n/home/nitin/spdk/spdk/dpdk/build/lib" \
    | sudo tee /etc/ld.so.conf.d/spdk.conf
sudo ldconfig
```

---

## 6. Build Instructions

### Step 1 — Build SPDK shared libraries (if not already done)

```bash
cd /home/nitin/spdk/spdk
./configure --with-shared          # CONFIG_SHARED=y
make -j$(nproc)
```

This produces `build/lib/libspdk_*.so` (76 libraries) and
`dpdk/build/lib/librte_*.so` (30 libraries).

### Step 2 — Build Layers 2 & 3

```bash
cd /home/nitin/spdk/spdkApp
mkdir -p build && cd build
cmake ..                           # detects SPDK paths automatically
make -j$(nproc)
```

**CMake cache variables** (override on the command line if needed):

| Variable | Default | Description |
|---|---|---|
| `SPDK_ROOT_DIR` | `/home/nitin/spdk/spdk` | SPDK source / build root |
| `SPDK_LIB_DIR` | `${SPDK_ROOT_DIR}/build/lib` | SPDK `.so` directory |
| `SPDK_INC_DIR` | `${SPDK_ROOT_DIR}/build/include` | SPDK headers |
| `DPDK_LIB_DIR` | `${SPDK_ROOT_DIR}/dpdk/build/lib` | DPDK `.so` directory |

Example with custom SPDK location:

```bash
cmake -DSPDK_ROOT_DIR=/opt/spdk ..
```

### Build Artifacts

| File | Size | Description |
|---|---|---|
| `build/mySPDKLib/libmySPDKLib.so.1.0.0` | 26 KB | NVMe wrapper shared library |
| `build/mySPDKLib/libmySPDKLib.so.1` | symlink | SONAME link |
| `build/mySPDKLib/libmySPDKLib.so` | symlink | Link-time name |
| `build/mySPDKApp/mySPDKApp` | 21 KB | NVMe exercise application |

Both artifacts have `RUNPATH` baked in so that `LD_LIBRARY_PATH` is not
required at runtime:

```
libmySPDKLib.so  RUNPATH: .../spdk/build/lib:.../dpdk/build/lib
mySPDKApp        RUNPATH: .../build/mySPDKLib:.../spdk/build/lib:.../dpdk/build/lib
```

---

## 7. Running the Application

```bash
# mySPDKApp accesses PCIe BARs and hugepages — root is required.
sudo /home/nitin/spdk/spdkApp/build/mySPDKApp/mySPDKApp
```

The application self-execs as the child process by passing `"child"` as
`argv[1]`:

```bash
# This is called internally by the parent via execl(); do not run manually.
sudo /home/nitin/spdk/spdkApp/build/mySPDKApp/mySPDKApp child
```

---

## 8. Detailed Execution Flow

```
Parent Process (mySPDKApp)
══════════════════════════════════════════════════════
│
│  ① init()
│    └─ spdk_env_init(&opts)          [DPDK EAL + hugepages]
│    └─ spdk_nvme_probe(PCIe)         [attach all PCIe NVMe controllers]
│    └─ attach_cb()  →  g_controllers[0] = { ctrlr, trid="0000:00:0e.0" }
│
│  ② identify_controllers()           [Step a]
│    └─ execute_admin_commands(0, Identify-Ctrl CNS=0x01, buf=4096B)
│       └─ spdk_zmalloc(4096) → dma_buf
│       └─ spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, cmd, dma_buf, cb)
│       └─ poll spdk_nvme_ctrlr_process_admin_completions() until done
│       └─ memcpy(dma_buf → buf)  →  spdk_free(dma_buf)
│    └─ print: BDF, Model, Serial, Firmware, VendorID, CtrlrID, NumNS
│
│  ③ identify_namespace()             [Step b]
│    └─ execute_admin_commands(0, Identify-NS CNS=0x00 NSID=1, buf=4096B)
│    └─ print: Size=52428800 sectors, Capacity, Sector=512B, Total=25GiB
│
│  ④ fflush(stdout)                   [flush before fork to preserve ordering]
│
│  ⑤ deinit()                         [Step c — prepare for fork]
│    └─ spdk_nvme_detach_async(ctrlr) → spdk_nvme_detach_poll()
│    └─ spdk_env_fini()
│       └─ vtophys_fini()  mem_map_fini()  pci_env_fini()
│       NOTE: DPDK EAL (hugepages) remains alive in this process
│
│  ⑥ fork()
│    └─ child PID spawned
│    │
│    │    ┌──────────────────────────────────────────────────────┐
│    │    │  Fork-copy child  →  execl(argv[0], argv[0], "child")│
│    │    └──────────────────────────────────────────────────────┘
│    │
│  ⑦ waitpid(child_pid)              [parent blocks here]
│
│                           ║  Child Process (mySPDKApp child)
│                           ║  ════════════════════════════════════════
│                           ║
│                           ║  ① init()                  [fresh process]
│                           ║    └─ spdk_env_init(&opts) [own DPDK EAL]
│                           ║    └─ spdk_nvme_probe()    [own attachment]
│                           ║
│                           ║  ② executeChild()          [Steps e1 + e2]
│                           ║
│                           ║    e1) Write "HelloWorld123456789" to SLBA 0
│                           ║        execute_io_commands(0, Write cmd)
│                           ║        └─ alloc qpair
│                           ║        └─ spdk_zmalloc(512) → dma_buf
│                           ║        └─ memcpy("HelloWorld..." → dma_buf)
│                           ║        └─ spdk_nvme_ctrlr_cmd_io_raw(Write)
│                           ║        └─ poll qpair completions
│                           ║        └─ spdk_free / free_qpair
│                           ║
│                           ║    e2) Read 25 bytes from SLBA 0
│                           ║        execute_io_commands(0, Read cmd)
│                           ║        └─ alloc qpair, dma_buf(512B)
│                           ║        └─ spdk_nvme_ctrlr_cmd_io_raw(Read)
│                           ║        └─ poll → memcpy(dma_buf → rbuf)
│                           ║        └─ print first 25 bytes
│                           ║           "HelloWorld123456789......"
│                           ║
│                           ║  ③ deinit() + exit(0)
│                           ║
│  ⑧ waitpid() returns                [child exited status 0]
│
│  ⑨ reinit()                         [Step c continued]
│    └─ spdk_env_dpdk_post_init(false)
│       └─ pci_env_init()   [re-register PCI drivers, re-scan bus]
│       └─ mem_map_init()   [rebuild address→PA mapping tables]
│       └─ vtophys_init()   [rebuild virtual-to-physical tables]  ← KEY FIX
│    └─ spdk_nvme_probe() → g_controllers[0] re-attached
│
│  ⑩ parent_read_slba0()             [Step d]
│    └─ execute_io_commands(0, Read SLBA=0 NLB=1, buf=512B)
│    └─ print first 20 bytes
│       "48 65 6c 6c 6f 57 6f 72 6c 64 31 32 33 34 35 36 37 38 39 00"
│       "HelloWorld123456789."         ← confirms child's write persisted
│
│  ⑪ deinit() + exit(0)
```

### Output (annotated)

```
[mySPDKApp] Running in PARENT mode (PID 105736)
[mySPDKLib] init() complete — 1 controller(s) found

════  NVMe Controllers found: 1  ════
[Controller 0]
  BDF (PCIe address) : 0000:00:0e.0         ← PCIe Bus:Device.Function
  Model Number       : ORCL-VBOX-NVME-VER12
  Serial Number      : VB1234-56789
  Firmware Revision  : 1.0
  PCI Vendor ID      : 0x80ee               ← Oracle/VirtualBox
  Controller ID      : 0x0000
  Max Namespaces     : 1

════  Namespace 1 on Controller 0  ════
  Namespace Size     : 52428800 sectors
  Sector Size        : 512 bytes
  Total Size         : 25.00 GiB

════  [Parent] Forking child process  ════

[mySPDKApp] Running in CHILD mode (PID 105738)
  [Child] Write: "HelloWorld123456789" → SLBA 0   ← Step e1
  [Child] Write successful
  [Child] Read: 25 bytes from SLBA 0              ← Step e2
  Hex  : 48 65 6c 6c 6f 57 6f 72 6c 64 ...
  ASCII: 'HelloWorld123456789......'

[Parent] Child exited with status 0
[Parent] Re-initialising SPDK after child exit …
[mySPDKLib] reinit() complete — 1 controller(s) found

════  [Parent] Read: first 20 bytes from SLBA 0  ════  ← Step d
  Hex  : 48 65 6c 6c 6f 57 6f 72 6c 64 31 32 33 34 35 36 37 38 39 00
  ASCII: HelloWorld123456789.    ← child's write is visible to parent ✓

[mySPDKApp] Parent exiting normally
```

---

## 9. NVMe Commands Used

All commands are issued via the raw command interface (`execute_admin_commands`
/ `execute_io_commands`) so the full 64-byte NVMe command structure is
controlled by the application.

### Identify Controller (Admin, opc=0x06)

```
CDW0  opc=0x06  (Identify)
CDW1  nsid=0x00 (not namespace-specific)
CDW10 CNS=0x01  (Controller data structure)
Buffer: 4096 bytes → struct spdk_nvme_ctrlr_data
```

### Identify Namespace (Admin, opc=0x06)

```
CDW0  opc=0x06  (Identify)
CDW1  nsid=0x01 (namespace 1)
CDW10 CNS=0x00  (Namespace data structure)
Buffer: 4096 bytes → struct spdk_nvme_ns_data
```

### Write (I/O, opc=0x01)

```
CDW0  opc=0x01  (Write)
CDW1  nsid=0x01
CDW10 SLBA[31:0]  = 0x00000000
CDW11 SLBA[63:32] = 0x00000000
CDW12 NLB-1       = 0x00000000  (1 logical block = 512 bytes)
Buffer: 512 bytes (one sector, payload = "HelloWorld123456789\0…")
```

### Read (I/O, opc=0x02)

```
CDW0  opc=0x02  (Read)
CDW1  nsid=0x01
CDW10 SLBA[31:0]  = 0x00000000
CDW11 SLBA[63:32] = 0x00000000
CDW12 NLB-1       = 0x00000000  (1 logical block = 512 bytes)
Buffer: 512 bytes → application inspects first 20 (parent) or 25 (child) bytes
```

---

## 10. Key Design Decisions

### Why `spdk_env_dpdk_post_init()` for reinit instead of `spdk_env_init(NULL)`?

The documented reinit path `spdk_env_init(NULL)` only calls
`pci_env_reinit()` internally.  After `spdk_env_fini()`, the vtophys and
mem_map tables are destroyed.  Since `pci_env_reinit()` does not call
`vtophys_init()` or `mem_map_init()`, every subsequent `spdk_vtophys()` call
returns `SPDK_VTOPHYS_ERROR`, causing the NVMe admin-queue construction to
fail with:

```
nvme_pcie_qpair_construct: spdk_vtophys(pqpair->cmd) failed
```

`spdk_env_dpdk_post_init(false)` is the public API designed for exactly this
situation: it re-runs all three post-init steps (PCI, mem-map, vtophys) on
top of the still-live DPDK EAL instance (the EAL is only torn down by
`rte_eal_cleanup()` which runs at process exit, not inside `spdk_env_fini()`).

### Why DMA buffers are allocated/freed per command?

Simplicity and fork safety.  A pool of long-lived DMA buffers would be faster
but complicates lifecycle management across deinit/reinit cycles.

### Why one I/O queue pair per `execute_io_commands()` call?

Single-threaded, low-frequency use case.  Keeping a queue pair alive across
the deinit/reinit boundary would require extra bookkeeping.

### Why `shm_id = -1`?

With `shm_id = -1` DPDK uses per-process anonymous hugepage memory without
creating named shared-memory files.  This prevents naming conflicts between
the parent and the execl'd child process, which are independent DPDK
instances.

---

## 11. Troubleshooting

### `libspdk_log.so.9.0: cannot open shared object file`

The SPDK `.so` files themselves have `RUNPATH` pointing only to the `isa-l`
directories.  Transitive dependencies of `libspdk_nvme.so` are not found
unless the SPDK build lib directory is in the ldconfig cache.

**Fix:**
```bash
echo -e "/home/nitin/spdk/spdk/build/lib\n/home/nitin/spdk/spdk/dpdk/build/lib" \
    | sudo tee /etc/ld.so.conf.d/spdk.conf
sudo ldconfig
```

### `No NVMe controllers found`

1. Check that the NVMe device is bound to `uio_pci_generic` or `vfio-pci`:
   ```bash
   sudo /home/nitin/spdk/spdk/scripts/setup.sh status
   ```
2. Check hugepages are allocated:
   ```bash
   cat /proc/meminfo | grep HugePages
   ```
3. Run as root (`sudo`).

### `spdk_vtophys(pqpair->cmd) failed` during `reinit()`

This appears if `spdk_env_init(NULL)` is used for reinit instead of
`spdk_env_dpdk_post_init(false)`.  The former skips vtophys re-initialisation.
See [Key Design Decisions](#10-key-design-decisions) above.

### Hugepage allocation failure

```bash
# Increase to 512 hugepages (1 GB)
echo 512 | sudo tee /proc/sys/vm/nr_hugepages
```

### Output appears out of order

When stdout is redirected to a pipe (e.g., `sudo … 2>&1 | tee log`), glibc
switches stdout to fully-buffered mode.  The parent's output accumulates in a
buffer and is flushed at exit, while the child (a separate process) flushes
its buffer at child exit — making child output appear first in the stream.

The application calls `fflush(stdout)` before `fork()` and before child exit
to ensure the console shows output in logical execution order.
