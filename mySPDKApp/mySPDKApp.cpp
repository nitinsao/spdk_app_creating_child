/**
 * mySPDKApp.cpp
 *
 * NVMe exercise application built on top of mySPDKLib.
 *
 * Execution modes
 * ---------------
 *   Normal (parent) mode  — invoked without arguments:
 *     a) Identify all NVMe controllers and print BDF + controller details.
 *     b) Identify the first namespace on controller 0 and print details.
 *     c) deinit(), fork(), execl() the same binary with "child" arg,
 *        waitpid() for the child, then reinit().
 *     d) Read the first 20 bytes from SLBA 0 on NS 1 of controller 0
 *        and print them (the child will have written "HelloWorld123456789").
 *
 *   Child mode  — invoked as:  mySPDKApp child
 *     e1) Write "HelloWorld123456789" to SLBA 0 on NS 1 of controller 0.
 *     e2) Read 25 bytes from SLBA 0 and print them.
 *
 * NVMe command usage
 * ------------------
 *   Admin – Identify Controller  (opc=0x06, CNS=0x01)
 *   Admin – Identify Namespace   (opc=0x06, CNS=0x00, NSID=nsid)
 *   I/O   – Read                 (opc=0x02, NSID, SLBA, NLB)
 *   I/O   – Write                (opc=0x01, NSID, SLBA, NLB)
 *
 * All commands are issued through execute_admin_commands() /
 * execute_io_commands() in mySPDKLib.
 */

#include "mySPDKLib.h"

#include <spdk/nvme_spec.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <unistd.h>
#include <sys/wait.h>

/* =========================================================================
 * Constants
 * ========================================================================= */

/** Controller and namespace we exercise (0-based index / 1-based NSID). */
static constexpr int      CTRLR_IDX = 0;
static constexpr uint32_t NSID      = 1;

/** Payload written by the child process. */
static const char CHILD_WRITE_PAYLOAD[] = "HelloWorld123456789";

/* =========================================================================
 * Helper: trim trailing spaces from a fixed-length NVMe ASCII field.
 * ========================================================================= */
static void trim_trailing_spaces(char *str, size_t len)
{
    for (int i = static_cast<int>(len) - 1; i >= 0; --i) {
        if (str[i] == ' ' || str[i] == '\0') {
            str[i] = '\0';
        } else {
            break;
        }
    }
}

/* =========================================================================
 * Step (a) — Identify all controllers, print details and BDF.
 * ========================================================================= */
static void identify_controllers(void)
{
    int num = get_num_controllers();
    printf("\n════════════════════════════════════════════════════\n");
    printf("  NVMe Controllers found: %d\n", num);
    printf("════════════════════════════════════════════════════\n");

    for (int i = 0; i < num; i++) {
        const struct spdk_nvme_transport_id *trid = get_controller_trid(i);
        printf("\n[Controller %d]\n", i);
        printf("  BDF (PCIe address) : %s\n", trid ? trid->traddr : "<unknown>");

        /* Issue Identify Controller admin command (CNS = 0x01). */
        struct spdk_nvme_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opc   = SPDK_NVME_OPC_IDENTIFY;
        cmd.cdw10 = 0x01; /* CNS = Identify Controller data structure */

        auto *cdata = static_cast<struct spdk_nvme_ctrlr_data *>(
            malloc(sizeof(struct spdk_nvme_ctrlr_data)));
        if (!cdata) {
            fprintf(stderr, "  [!] malloc failed\n");
            continue;
        }
        memset(cdata, 0, sizeof(*cdata));

        int rc = execute_admin_commands(i, &cmd, cdata,
                                        sizeof(struct spdk_nvme_ctrlr_data));
        if (rc != 0) {
            fprintf(stderr,
                    "  [!] Identify Controller failed (rc=%d)\n", rc);
            free(cdata);
            continue;
        }

        /* Model Number — 40-byte ASCII, space padded. */
        char mn[41];
        memcpy(mn, cdata->mn, 40);
        mn[40] = '\0';
        trim_trailing_spaces(mn, 40);

        /* Serial Number — 20-byte ASCII, space padded. */
        char sn[21];
        memcpy(sn, cdata->sn, 20);
        sn[20] = '\0';
        trim_trailing_spaces(sn, 20);

        /* Firmware Revision — 8-byte ASCII. */
        char fr[9];
        memcpy(fr, cdata->fr, 8);
        fr[8] = '\0';
        trim_trailing_spaces(fr, 8);

        printf("  Model Number       : %s\n",  mn);
        printf("  Serial Number      : %s\n",  sn);
        printf("  Firmware Revision  : %s\n",  fr);
        printf("  PCI Vendor ID      : 0x%04x\n", cdata->vid);
        printf("  Controller ID      : 0x%04x\n", cdata->cntlid);
        printf("  Max Namespaces     : %u\n",
               get_num_namespaces(i));

        free(cdata);
    }
}

/* =========================================================================
 * Step (b) — Identify NS 1 on controller 0, print details.
 * ========================================================================= */
static void identify_namespace(void)
{
    printf("\n════════════════════════════════════════════════════\n");
    printf("  Namespace %u on Controller %d\n", NSID, CTRLR_IDX);
    printf("════════════════════════════════════════════════════\n");

    /* Identify Namespace admin command (CNS = 0x00, NSID = 1). */
    struct spdk_nvme_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opc   = SPDK_NVME_OPC_IDENTIFY;
    cmd.nsid  = NSID;
    cmd.cdw10 = 0x00; /* CNS = Identify Namespace data structure */

    auto *nsdata = static_cast<struct spdk_nvme_ns_data *>(
        malloc(sizeof(struct spdk_nvme_ns_data)));
    if (!nsdata) {
        fprintf(stderr, "[!] malloc failed\n");
        return;
    }
    memset(nsdata, 0, sizeof(*nsdata));

    int rc = execute_admin_commands(CTRLR_IDX, &cmd, nsdata,
                                    sizeof(struct spdk_nvme_ns_data));
    if (rc != 0) {
        fprintf(stderr, "[!] Identify Namespace failed (rc=%d)\n", rc);
        free(nsdata);
        return;
    }

    uint32_t sector_size = get_sector_size(CTRLR_IDX, NSID);

    printf("  Namespace Size     : %llu sectors\n",
           (unsigned long long)nsdata->nsze);
    printf("  Namespace Capacity : %llu sectors\n",
           (unsigned long long)nsdata->ncap);
    printf("  Namespace Util.    : %llu sectors\n",
           (unsigned long long)nsdata->nuse);
    printf("  Sector Size        : %u bytes\n", sector_size);
    printf("  Total Size         : %.2f GiB\n",
           (double)nsdata->nsze * sector_size / (1024.0 * 1024.0 * 1024.0));

    free(nsdata);
}

/* =========================================================================
 * Step (d) — Read first 20 bytes from SLBA 0 (parent, after child exits).
 * ========================================================================= */
static void parent_read_slba0(void)
{
    printf("\n════════════════════════════════════════════════════\n");
    printf("  [Parent] Read: first 20 bytes from SLBA 0\n");
    printf("════════════════════════════════════════════════════\n");

    uint32_t sector_size = get_sector_size(CTRLR_IDX, NSID);
    if (sector_size == 0) {
        fprintf(stderr, "[!] Could not determine sector size\n");
        return;
    }

    /* Allocate a sector-aligned read buffer. */
    auto *buf = static_cast<uint8_t *>(malloc(sector_size));
    if (!buf) {
        fprintf(stderr, "[!] malloc failed\n");
        return;
    }
    memset(buf, 0, sector_size);

    /* Build NVMe Read command. */
    struct spdk_nvme_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opc   = SPDK_NVME_OPC_READ; /* 0x02 */
    cmd.nsid  = NSID;
    cmd.cdw10 = 0; /* SLBA[31:0]  = 0 */
    cmd.cdw11 = 0; /* SLBA[63:32] = 0 */
    cmd.cdw12 = 0; /* NLB - 1     = 0  (read exactly 1 sector) */

    int rc = execute_io_commands(CTRLR_IDX, &cmd, buf, sector_size);
    if (rc != 0) {
        fprintf(stderr, "[!] Read failed (rc=%d)\n", rc);
        free(buf);
        return;
    }

    /* Print first 20 bytes as hex and as printable ASCII. */
    printf("  Hex  : ");
    for (int i = 0; i < 20; i++) {
        printf("%02x ", buf[i]);
    }
    printf("\n");

    printf("  ASCII: ");
    for (int i = 0; i < 20; i++) {
        printf("%c", isprint(buf[i]) ? buf[i] : '.');
    }
    printf("\n");

    free(buf);
}

/* =========================================================================
 * Step (e) — Child function: write HelloWorld, read 25 bytes back.
 * ========================================================================= */
static void executeChild(void)
{
    printf("\n════════════════════════════════════════════════════\n");
    printf("  [Child] executeChild() — PID %d\n", (int)getpid());
    printf("════════════════════════════════════════════════════\n");

    uint32_t sector_size = get_sector_size(CTRLR_IDX, NSID);
    if (sector_size == 0) {
        fprintf(stderr, "[Child] Could not determine sector size\n");
        return;
    }
    printf("  [Child] Sector size: %u bytes\n", sector_size);

    /* ---- e1: Write "HelloWorld123456789" to SLBA 0 ---- */
    printf("\n  [Child] Write: \"%s\" → SLBA 0\n", CHILD_WRITE_PAYLOAD);

    auto *wbuf = static_cast<uint8_t *>(malloc(sector_size));
    if (!wbuf) {
        fprintf(stderr, "[Child] malloc failed\n");
        return;
    }
    memset(wbuf, 0, sector_size);

    size_t payload_len = strlen(CHILD_WRITE_PAYLOAD);
    memcpy(wbuf, CHILD_WRITE_PAYLOAD, payload_len);

    struct spdk_nvme_cmd wcmd;
    memset(&wcmd, 0, sizeof(wcmd));
    wcmd.opc   = SPDK_NVME_OPC_WRITE; /* 0x01 */
    wcmd.nsid  = NSID;
    wcmd.cdw10 = 0; /* SLBA[31:0]  = 0 */
    wcmd.cdw11 = 0; /* SLBA[63:32] = 0 */
    wcmd.cdw12 = 0; /* NLB - 1     = 0  (write 1 sector) */

    int rc = execute_io_commands(CTRLR_IDX, &wcmd, wbuf, sector_size);
    if (rc != 0) {
        fprintf(stderr, "[Child] Write failed (rc=%d)\n", rc);
        free(wbuf);
        return;
    }
    printf("  [Child] Write successful\n");
    free(wbuf);

    /* ---- e2: Read 25 bytes from SLBA 0 (read whole sector, show 25) ---- */
    printf("\n  [Child] Read: 25 bytes from SLBA 0\n");

    auto *rbuf = static_cast<uint8_t *>(malloc(sector_size));
    if (!rbuf) {
        fprintf(stderr, "[Child] malloc failed\n");
        return;
    }
    memset(rbuf, 0, sector_size);

    struct spdk_nvme_cmd rcmd;
    memset(&rcmd, 0, sizeof(rcmd));
    rcmd.opc   = SPDK_NVME_OPC_READ; /* 0x02 */
    rcmd.nsid  = NSID;
    rcmd.cdw10 = 0;
    rcmd.cdw11 = 0;
    rcmd.cdw12 = 0;

    rc = execute_io_commands(CTRLR_IDX, &rcmd, rbuf, sector_size);
    if (rc != 0) {
        fprintf(stderr, "[Child] Read failed (rc=%d)\n", rc);
        free(rbuf);
        return;
    }

    /* Print first 25 bytes as hex and ASCII. */
    printf("  Hex  : ");
    for (int i = 0; i < 25; i++) {
        printf("%02x ", rbuf[i]);
    }
    printf("\n");

    printf("  ASCII: '");
    for (int i = 0; i < 25; i++) {
        printf("%c", isprint(rbuf[i]) ? rbuf[i] : '.');
    }
    printf("'\n");

    free(rbuf);
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(int argc, char *argv[])
{
    bool child_mode = (argc > 1 && strcmp(argv[1], "child") == 0);

    /* ------------------------------------------------------------------
     * Child mode: fresh SPDK init, execute child tasks, deinit, exit.
     * ------------------------------------------------------------------ */
    if (child_mode) {
        printf("\n[mySPDKApp] Running in CHILD mode (PID %d)\n",
               (int)getpid());

        if (init() != 0) {
            fprintf(stderr, "[Child] init() failed\n");
            return 1;
        }
        if (get_num_controllers() == 0) {
            fprintf(stderr, "[Child] No NVMe controllers found\n");
            deinit();
            return 1;
        }

        executeChild();

        fflush(stdout);
        deinit();
        printf("[mySPDKApp] Child exiting normally\n");
        fflush(stdout);
        return 0;
    }

    /* ------------------------------------------------------------------
     * Parent (normal) mode.
     * ------------------------------------------------------------------ */
    printf("[mySPDKApp] Running in PARENT mode (PID %d)\n",
           (int)getpid());

    /* ---- Step a: Initialize and identify controllers ---- */
    if (init() != 0) {
        fprintf(stderr, "[Parent] init() failed\n");
        return 1;
    }
    if (get_num_controllers() == 0) {
        fprintf(stderr, "[Parent] No NVMe controllers found\n");
        deinit();
        return 1;
    }

    identify_controllers();    /* step (a) */
    identify_namespace();      /* step (b) */

    /* ---- Step c: deinit → fork → execl child → waitpid → reinit ---- */
    printf("\n════════════════════════════════════════════════════\n");
    printf("  [Parent] Forking child process\n");
    printf("════════════════════════════════════════════════════\n");

    /*
     * Flush stdout before fork so buffered output is not duplicated in
     * both parent and child and the console shows lines in execution order.
     */
    fflush(stdout);

    /*
     * Detach all NVMe controllers and tear down the DPDK SPDK post-init
     * layer before fork() so that the execl'd child can independently
     * initialise SPDK and attach the same PCIe devices.
     * (The DPDK EAL itself remains alive; reinit() will re-run the
     * SPDK post-init on top of it after the child has finished.)
     */
    deinit();

    pid_t pid = fork();
    if (pid < 0) {
        perror("[Parent] fork");
        return 1;
    }

    if (pid == 0) {
        /*
         * Child branch (fork copy): replace this process image with a
         * fresh invocation of mySPDKApp passing "child" as the first
         * argument.
         */
        execl(argv[0], argv[0], "child", (char *)nullptr);
        /* execl only returns on error. */
        perror("[Child-fork] execl");
        _exit(1);
    }

    /* Parent: wait for child to finish before re-attaching the device. */
    printf("[Parent] Waiting for child PID %d to complete …\n", (int)pid);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    printf("[Parent] Child exited with status %d\n",
           WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1);

    /*
     * Re-initialise SPDK in the parent process.  The child has already
     * released the PCIe device, so the parent can attach it again.
     */
    printf("\n[Parent] Re-initialising SPDK after child exit …\n");
    if (reinit() != 0) {
        fprintf(stderr, "[Parent] reinit() failed\n");
        return 1;
    }
    if (get_num_controllers() == 0) {
        fprintf(stderr, "[Parent] No NVMe controllers found after reinit\n");
        deinit();
        return 1;
    }

    /* ---- Step d: Read first 20 bytes from SLBA 0 ---- */
    parent_read_slba0();

    deinit();
    printf("\n[mySPDKApp] Parent exiting normally\n");
    return 0;
}
