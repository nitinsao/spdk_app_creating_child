/**
 * mySPDKLib.h
 *
 * Public API for mySPDKLib — a thin C++ wrapper around the SPDK/DPDK NVMe
 * user-space driver that provides:
 *   - PCIe NVMe controller discovery (init / deinit / reinit)
 *   - Raw admin-command submission  (execute_admin_commands)
 *   - Raw I/O-command submission    (execute_io_commands)
 *
 * The library manages all DMA buffer allocation internally; callers may pass
 * ordinary heap pointers as data buffers.
 *
 * Exported with C linkage so the .so can be consumed from both C and C++.
 */

#pragma once

#include <cstdint>

/* SPDK types used in the public API ---------------------------------------- */
#include <spdk/nvme.h>      /* spdk_nvme_ctrlr, spdk_nvme_transport_id      */
#include <spdk/nvme_spec.h> /* spdk_nvme_cmd                                 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * init()
 *
 * Initialise the SPDK / DPDK environment, then probe and attach every PCIe
 * NVMe controller visible to the host.  The discovered controllers are stored
 * in a process-local table and addressed by a 0-based index in subsequent
 * calls.
 *
 * This function is idempotent: a second call while already initialised
 * returns 0 without re-initialising.
 *
 * @return  0 on success, negative errno on failure.
 */
int init(void);

/**
 * deinit()
 *
 * Detach every previously attached NVMe controller and tear down the SPDK /
 * DPDK environment.
 *
 * Intended use: call immediately before fork() so that the child process can
 * independently re-attach the same devices (see reinit()).  Both the parent
 * and the child must NOT use any SPDK API between deinit() and reinit().
 */
void deinit(void);

/**
 * reinit()
 *
 * Re-initialise the SPDK / DPDK environment (using the same options as the
 * first init() call) and re-probe all PCIe NVMe controllers.
 *
 * Must be called only after deinit() has been called in the same process.
 * Typically called by the parent after waitpid() returns, or by the child
 * if it was not execl'd and needs to reacquire the devices.
 *
 * @return  0 on success, negative errno on failure.
 */
int reinit(void);

/**
 * get_num_controllers()
 *
 * @return  Number of NVMe controllers currently attached.
 */
int get_num_controllers(void);

/**
 * get_controller()
 *
 * @param idx  0-based controller index.
 * @return     Opaque controller handle, or NULL if idx is out of range.
 */
struct spdk_nvme_ctrlr *get_controller(int idx);

/**
 * get_controller_trid()
 *
 * Retrieve the transport identifier (which contains the BDF address for
 * PCIe devices) of a discovered controller.
 *
 * @param idx  0-based controller index.
 * @return     Pointer to the transport ID, or NULL if idx is out of range.
 *             The pointer remains valid until deinit() is called.
 */
const struct spdk_nvme_transport_id *get_controller_trid(int idx);

/**
 * get_num_namespaces()
 *
 * Return the total number of namespaces (active + inactive) supported by
 * the controller.  Namespace IDs run from 1 to this value.
 *
 * @param ctrlr_idx  0-based controller index.
 * @return           Namespace count, or 0 on error.
 */
uint32_t get_num_namespaces(int ctrlr_idx);

/**
 * get_sector_size()
 *
 * Return the formatted LBA size (sector size in bytes) for the given
 * namespace on the given controller.
 *
 * @param ctrlr_idx  0-based controller index.
 * @param nsid       1-based namespace ID.
 * @return           Sector size in bytes, or 0 on error.
 */
uint32_t get_sector_size(int ctrlr_idx, uint32_t nsid);

/**
 * execute_admin_commands()
 *
 * Submit a raw NVMe Admin command via spdk_nvme_ctrlr_cmd_admin_raw() and
 * block (by polling) until the command completes.
 *
 * The caller fills in the command opcode (cmd->opc), NSID (cmd->nsid) and
 * command-specific dwords (cmd->cdw10 … cmd->cdw15).  The DPTR (data-
 * pointer) fields are populated internally from the DMA buffer that the
 * library allocates for each call; the caller's buf/len values describe a
 * normal heap buffer whose contents are copied to/from the DMA region.
 *
 * For read-type admin commands (e.g. Identify) the device response is
 * written back into buf before returning.  For write-type admin commands
 * (e.g. Set Features with a data payload) the content of buf is first
 * copied into the DMA region before submission.
 *
 * @param ctrlr_idx  0-based controller index.
 * @param cmd        Pointer to the NVMe command structure (modified in place
 *                   by SPDK to fill the CID; DPTR is overwritten).
 * @param buf        Caller-allocated data buffer (read/write, depending on
 *                   the command direction).  May be NULL if len == 0.
 * @param len        Size of buf in bytes.
 * @return           0 on success, negative on submission failure,
 *                   or 1 if the NVMe completion status indicates an error.
 */
int execute_admin_commands(int ctrlr_idx, struct spdk_nvme_cmd *cmd,
                           void *buf, uint32_t len);

/**
 * execute_io_commands()
 *
 * Submit a raw NVMe I/O command via spdk_nvme_ctrlr_cmd_io_raw() and block
 * (by polling) until the command completes.
 *
 * The caller must fill in:
 *   cmd->opc           — NVMe I/O opcode (e.g. 0x01 = Write, 0x02 = Read)
 *   cmd->nsid          — 1-based namespace ID
 *   cmd->cdw10         — SLBA bits [31:0]
 *   cmd->cdw11         — SLBA bits [63:32]
 *   cmd->cdw12         — Number of Logical Blocks minus 1 (NLB field)
 *
 * The DPTR is populated internally.  buf must be at least
 * (NLB + 1) × sector_size bytes; len should equal this value.
 *
 * An I/O queue pair is allocated and freed for each call, which keeps the
 * implementation simple and fork-safe.
 *
 * @param ctrlr_idx  0-based controller index.
 * @param cmd        Pointer to the NVMe command structure.
 * @param buf        Caller-allocated data buffer.  For Read commands the
 *                   device data is copied here on completion; for Write
 *                   commands the buffer is copied to DMA memory before
 *                   submission.  May be NULL if len == 0.
 * @param len        Size of buf in bytes (must be a multiple of the sector
 *                   size for I/O commands).
 * @return           0 on success, negative on submission failure,
 *                   or 1 if the NVMe completion status indicates an error.
 */
int execute_io_commands(int ctrlr_idx, struct spdk_nvme_cmd *cmd,
                        void *buf, uint32_t len);

#ifdef __cplusplus
} /* extern "C" */
#endif
