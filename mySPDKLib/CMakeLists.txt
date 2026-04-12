# ============================================================================
# mySPDKLib — shared library wrapping SPDK/DPDK NVMe driver
# ============================================================================
cmake_minimum_required(VERSION 3.16)

# --------------------------------------------------------------------------
# Source files
# --------------------------------------------------------------------------
add_library(mySPDKLib SHARED
    mySPDKLib.cpp
)

# --------------------------------------------------------------------------
# Include paths
# --------------------------------------------------------------------------
target_include_directories(mySPDKLib
    PUBLIC
        # Own headers (consumers of the lib get this automatically)
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        # SPDK public headers
        ${SPDK_INC_DIR}
)

# --------------------------------------------------------------------------
# Compile options
# --------------------------------------------------------------------------
target_compile_options(mySPDKLib PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    # Required by DPDK/SPDK to enable GNU extensions
    -D_GNU_SOURCE
)

# --------------------------------------------------------------------------
# Link libraries (order matters for static archives in the linker)
# --------------------------------------------------------------------------
#
# We link against the SPDK / DPDK shared libraries that libspdk_nvme.so
# and libspdk_env_dpdk.so depend on.  The exact set is derived from
# pkg-config (spdk_nvme + spdk_env_dpdk) which transitively pulls in
# all required DPDK rte_* libraries.
#
target_link_libraries(mySPDKLib
    PRIVATE
        # Core NVMe driver
        -L${SPDK_LIB_DIR}
        spdk_nvme

        # SPDK support libraries (pulled in by spdk_nvme's NEEDED entries)
        spdk_env_dpdk
        spdk_log
        spdk_util
        spdk_trace
        spdk_dma
        spdk_sock
        spdk_sock_posix
        spdk_keyring
        spdk_rpc
        spdk_jsonrpc
        spdk_json
        spdk_thread
        spdk_init

        # DPDK EAL + required PMD / service libraries
        -L${DPDK_LIB_DIR}
        rte_eal
        rte_mempool
        rte_mempool_ring
        rte_ring
        rte_pci
        rte_bus_pci
        rte_kvargs
        rte_log
        rte_telemetry
        rte_argparse
        rte_power
        rte_timer
        rte_hash
        rte_mbuf
        rte_net
        rte_ethdev
        rte_cryptodev
        rte_dmadev
        rte_rcu
        rte_meter
        rte_vhost
        rte_bus_vdev

        # OpenSSL — libspdk_nvme / libspdk_env_dpdk use EVP_* symbols but
        # do not declare libssl/libcrypto in their DT_NEEDED entries.
        ssl
        crypto

        # System libraries
        -lfuse3
        -lnuma
        -ldl
        -lpthread
        -luuid
)

# --------------------------------------------------------------------------
# Runtime library search paths (RPATH)
#
# Embed the absolute paths to the SPDK and DPDK build directories so that
# both mySPDKLib.so and any executable that loads it can locate the
# libspdk_*.so / librte_*.so files without LD_LIBRARY_PATH.
# --------------------------------------------------------------------------
set_target_properties(mySPDKLib PROPERTIES
    # Visible symbol visibility default (export everything by default for now)
    CXX_VISIBILITY_PRESET  default
    VISIBILITY_INLINES_HIDDEN OFF

    # Make the SONAME explicit so that linkers embed the correct DT_SONAME
    VERSION   1.0.0
    SOVERSION 1

    # RPATH baked into the .so at build time
    BUILD_RPATH  "${SPDK_LIB_DIR};${DPDK_LIB_DIR}"
    INSTALL_RPATH "${SPDK_LIB_DIR};${DPDK_LIB_DIR}"

    # Do NOT strip the RPATH on install
    INSTALL_RPATH_USE_LINK_PATH TRUE
)
