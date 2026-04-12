/**
 * mySPDKLib.cpp
 *
 * Implementation of the mySPDKLib shared library.
 *
 * Design notes
 * ============
 * • The SPDK NVMe driver is completely asynchronous.  Every submit function
 *   (spdk_nvme_ctrlr_cmd_admin_raw, spdk_nvme_ctrlr_cmd_io_raw) only enqueues
 *   the command; the caller must poll for completions.  This library wraps
 *   the async model behind a synchronous interface by spinning on a "done"
 *   flag in the completion callback.
 *
 * • NVMe commands operate on DMA-capable (physically contiguous) memory.
 *   Ordinary malloc() buffers cannot be passed directly to the driver.  For
 *   each call the library allocates a temporary DMA buffer via spdk_zmalloc(),
 *   copies data in/out around the command submission, and frees the buffer on
 *   return.  This is slightly less efficient than long-lived DMA buffers but
 *   keeps the public API simple and fork-safe.
 *
 * • I/O queue pairs are allocated per execute_io_commands() call and freed
 *   immediately after the completion is polled.  This avoids the need for the
 *   caller to manage qpair lifecycle and is safe across fork/reinit cycles.
 *
 * • Fork safety: deinit() detaches all controllers and calls spdk_env_fini().
 *   reinit() calls spdk_env_init(NULL) (same-process reinitialisation) and
 *   re-probes.  Because the execl() child is a fresh process image it calls
 *   the full init() path instead of reinit().
 */

#include "mySPDKLib.h"

#include <spdk/env.h>
#include <spdk/env_dpdk.h>  /* spdk_env_dpdk_post_init / spdk_env_dpdk_post_fini */
#include <spdk/log.h>

#include <cstring>
#include <cstdio>
#include <vector>

/* =========================================================================
 * Internal types
 * ========================================================================= */

/** One discovered NVMe controller plus its transport address (BDF). */
struct ControllerEntry {
    struct spdk_nvme_ctrlr       *ctrlr;
    struct spdk_nvme_transport_id trid;
};

/** Per-command completion context shared with the callback. */
struct CmdContext {
    volatile bool done;   /**< Set to true by the completion callback.   */
    int           status; /**< 0 = success, 1 = NVMe error after completion. */
};

/* =========================================================================
 * Process-local state
 * ========================================================================= */

static std::vector<ControllerEntry> g_controllers;
static bool                         g_env_initialized = false;
static struct spdk_env_opts         g_env_opts        = {};

/* =========================================================================
 * SPDK probe callbacks (internal)
 * ========================================================================= */

/**
 * probe_cb — called once per controller found during spdk_nvme_probe().
 * Return true to attach; we accept every PCIe NVMe device we see.
 */
static bool probe_cb(void                             *cb_ctx,
                     const struct spdk_nvme_transport_id *trid,
                     struct spdk_nvme_ctrlr_opts      *opts)
{
    (void)cb_ctx;
    (void)opts;
    printf("[mySPDKLib] Probe: found controller at %s — attaching\n",
           trid->traddr);
    return true; /* accept */
}

/**
 * attach_cb — called once a controller has been successfully attached.
 * Stores the handle and its transport ID for later use.
 */
static void attach_cb(void                             *cb_ctx,
                      const struct spdk_nvme_transport_id *trid,
                      struct spdk_nvme_ctrlr           *ctrlr,
                      const struct spdk_nvme_ctrlr_opts *opts)
{
    (void)cb_ctx;
    (void)opts;

    ControllerEntry entry{};
    entry.ctrlr = ctrlr;
    entry.trid  = *trid;
    g_controllers.push_back(entry);

    printf("[mySPDKLib] Attached controller: BDF=%s\n", trid->traddr);
}

/* =========================================================================
 * Command completion callback (internal)
 * ========================================================================= */

static void cmd_completion_cb(void *cb_arg, const struct spdk_nvme_cpl *cpl)
{
    CmdContext *ctx = static_cast<CmdContext *>(cb_arg);
    ctx->status     = spdk_nvme_cpl_is_error(cpl) ? 1 : 0;
    ctx->done       = true;
}

/* =========================================================================
 * Public API implementation
 * ========================================================================= */

int init(void)
{
    if (g_env_initialized) {
        printf("[mySPDKLib] init() called while already initialised — skipping\n");
        return 0;
    }

    /* ---- Initialise DPDK / SPDK environment ---- */
    g_env_opts.opts_size = sizeof(g_env_opts);
    spdk_env_opts_init(&g_env_opts);
    g_env_opts.name   = "mySPDKApp";
    g_env_opts.shm_id = -1; /* anonymous hugepages — no cross-process conflict */

    if (spdk_env_init(&g_env_opts) < 0) {
        fprintf(stderr, "[mySPDKLib] spdk_env_init() failed\n");
        return -1;
    }
    g_env_initialized = true;

    /* ---- Probe all PCIe NVMe controllers ---- */
    struct spdk_nvme_transport_id trid = {};
    spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);

    int rc = spdk_nvme_probe(&trid, nullptr, probe_cb, attach_cb, nullptr);
    if (rc != 0) {
        fprintf(stderr, "[mySPDKLib] spdk_nvme_probe() failed: rc=%d\n", rc);
        return rc;
    }

    printf("[mySPDKLib] init() complete — %zu controller(s) found\n",
           g_controllers.size());
    return 0;
}

void deinit(void)
{
    /* Detach all controllers asynchronously, then poll until done. */
    struct spdk_nvme_detach_ctx *detach_ctx = nullptr;

    for (auto &entry : g_controllers) {
        if (entry.ctrlr) {
            spdk_nvme_detach_async(entry.ctrlr, &detach_ctx);
            entry.ctrlr = nullptr;
        }
    }
    if (detach_ctx) {
        spdk_nvme_detach_poll(detach_ctx);
    }
    g_controllers.clear();

    /* Release DPDK hugepages and other env resources. */
    if (g_env_initialized) {
        spdk_env_fini();
        g_env_initialized = false;
    }

    printf("[mySPDKLib] deinit() complete\n");
}

int reinit(void)
{
    if (g_env_initialized) {
        fprintf(stderr,
                "[mySPDKLib] reinit() called while already initialised; "
                "call deinit() first\n");
        return -1;
    }

    /*
     * After deinit(), spdk_env_fini() has destroyed the vtophys and
     * mem_map tables, but the DPDK EAL (hugepages, memory segments) is
     * still alive — rte_eal_cleanup() is only called at process exit.
     *
     * spdk_env_init(NULL) only calls pci_env_reinit() and therefore does
     * NOT restore vtophys, causing spdk_vtophys() to fail for any DMA
     * allocation the NVMe driver tries to make.
     *
     * spdk_env_dpdk_post_init(false) is the correct call: it re-runs the
     * full SPDK post-init sequence (pci_env_init, mem_map_init,
     * vtophys_init) on top of the still-alive DPDK EAL instance.
     */
    int rc = spdk_env_dpdk_post_init(/*legacy_mem=*/false);
    if (rc != 0) {
        fprintf(stderr,
                "[mySPDKLib] spdk_env_dpdk_post_init() failed during "
                "reinit: rc=%d\n", rc);
        return rc;
    }
    g_env_initialized = true;

    /* Re-probe controllers. */
    struct spdk_nvme_transport_id trid = {};
    spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);

    int probe_rc = spdk_nvme_probe(&trid, nullptr, probe_cb, attach_cb, nullptr);
    if (probe_rc != 0) {
        fprintf(stderr,
                "[mySPDKLib] spdk_nvme_probe() failed during reinit: rc=%d\n",
                probe_rc);
        return probe_rc;
    }

    printf("[mySPDKLib] reinit() complete — %zu controller(s) found\n",
           g_controllers.size());
    return 0;
}

int get_num_controllers(void)
{
    return static_cast<int>(g_controllers.size());
}

struct spdk_nvme_ctrlr *get_controller(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(g_controllers.size())) {
        return nullptr;
    }
    return g_controllers[idx].ctrlr;
}

const struct spdk_nvme_transport_id *get_controller_trid(int idx)
{
    if (idx < 0 || idx >= static_cast<int>(g_controllers.size())) {
        return nullptr;
    }
    return &g_controllers[idx].trid;
}

uint32_t get_num_namespaces(int ctrlr_idx)
{
    struct spdk_nvme_ctrlr *ctrlr = get_controller(ctrlr_idx);
    if (!ctrlr) {
        return 0;
    }
    return spdk_nvme_ctrlr_get_num_ns(ctrlr);
}

uint32_t get_sector_size(int ctrlr_idx, uint32_t nsid)
{
    struct spdk_nvme_ctrlr *ctrlr = get_controller(ctrlr_idx);
    if (!ctrlr) {
        return 0;
    }
    struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
    if (!ns || !spdk_nvme_ns_is_active(ns)) {
        return 0;
    }
    return spdk_nvme_ns_get_sector_size(ns);
}

int execute_admin_commands(int ctrlr_idx, struct spdk_nvme_cmd *cmd,
                           void *buf, uint32_t len)
{
    if (ctrlr_idx < 0 ||
        ctrlr_idx >= static_cast<int>(g_controllers.size())) {
        fprintf(stderr,
                "[mySPDKLib] execute_admin_commands: invalid ctrlr_idx %d\n",
                ctrlr_idx);
        return -1;
    }

    struct spdk_nvme_ctrlr *ctrlr = g_controllers[ctrlr_idx].ctrlr;

    /* Allocate a DMA-capable buffer and copy caller data into it. */
    void *dma_buf = nullptr;
    if (len > 0 && buf != nullptr) {
        dma_buf = spdk_zmalloc(len, /*align=*/4096, /*phys=*/nullptr,
                               SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!dma_buf) {
            fprintf(stderr,
                    "[mySPDKLib] execute_admin_commands: "
                    "spdk_zmalloc(%u) failed\n", len);
            return -1;
        }
        /*
         * Copy caller buffer into DMA region.  For read-type commands the
         * device overwrites this; for write-type commands the data is already
         * in place.
         */
        memcpy(dma_buf, buf, len);
    }

    /* Submit the command (SPDK will set the DPTR from dma_buf / len). */
    CmdContext ctx{ false, 0 };
    int rc = spdk_nvme_ctrlr_cmd_admin_raw(ctrlr, cmd,
                                           dma_buf, len,
                                           cmd_completion_cb, &ctx);
    if (rc != 0) {
        fprintf(stderr,
                "[mySPDKLib] spdk_nvme_ctrlr_cmd_admin_raw() failed: rc=%d\n",
                rc);
        if (dma_buf) {
            spdk_free(dma_buf);
        }
        return rc;
    }

    /* Busy-poll until the admin completion queue processes the entry. */
    while (!ctx.done) {
        spdk_nvme_ctrlr_process_admin_completions(ctrlr);
    }

    /* Copy DMA result back to caller's buffer (important for read commands). */
    if (dma_buf && buf) {
        memcpy(buf, dma_buf, len);
        spdk_free(dma_buf);
    }

    return ctx.status; /* 0 = success, 1 = NVMe error */
}

int execute_io_commands(int ctrlr_idx, struct spdk_nvme_cmd *cmd,
                        void *buf, uint32_t len)
{
    if (ctrlr_idx < 0 ||
        ctrlr_idx >= static_cast<int>(g_controllers.size())) {
        fprintf(stderr,
                "[mySPDKLib] execute_io_commands: invalid ctrlr_idx %d\n",
                ctrlr_idx);
        return -1;
    }

    struct spdk_nvme_ctrlr *ctrlr = g_controllers[ctrlr_idx].ctrlr;

    /* Allocate an I/O queue pair for this operation. */
    struct spdk_nvme_qpair *qpair =
        spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, nullptr, 0);
    if (!qpair) {
        fprintf(stderr,
                "[mySPDKLib] execute_io_commands: "
                "spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
        return -1;
    }

    /* Allocate DMA buffer and copy caller data (required for Write). */
    void *dma_buf = nullptr;
    if (len > 0 && buf != nullptr) {
        dma_buf = spdk_zmalloc(len, /*align=*/4096, /*phys=*/nullptr,
                               SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!dma_buf) {
            fprintf(stderr,
                    "[mySPDKLib] execute_io_commands: "
                    "spdk_zmalloc(%u) failed\n", len);
            spdk_nvme_ctrlr_free_io_qpair(qpair);
            return -1;
        }
        memcpy(dma_buf, buf, len);
    }

    /* Submit the I/O command. */
    CmdContext ctx{ false, 0 };
    int rc = spdk_nvme_ctrlr_cmd_io_raw(ctrlr, qpair, cmd,
                                        dma_buf, len,
                                        cmd_completion_cb, &ctx);
    if (rc != 0) {
        fprintf(stderr,
                "[mySPDKLib] spdk_nvme_ctrlr_cmd_io_raw() failed: rc=%d\n",
                rc);
        if (dma_buf) {
            spdk_free(dma_buf);
        }
        spdk_nvme_ctrlr_free_io_qpair(qpair);
        return rc;
    }

    /* Busy-poll the I/O completion queue. */
    while (!ctx.done) {
        spdk_nvme_qpair_process_completions(qpair, /*max_completions=*/0);
    }

    /* Copy the DMA result back to caller (important for Read commands). */
    if (dma_buf && buf) {
        memcpy(buf, dma_buf, len);
        spdk_free(dma_buf);
    }

    /* Release the queue pair. */
    spdk_nvme_ctrlr_free_io_qpair(qpair);

    return ctx.status; /* 0 = success, 1 = NVMe error */
}
