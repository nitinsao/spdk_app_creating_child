# ============================================================================
# mySPDKApp — NVMe exercise application using mySPDKLib
# ============================================================================
cmake_minimum_required(VERSION 3.16)

# --------------------------------------------------------------------------
# Executable
# --------------------------------------------------------------------------
add_executable(mySPDKApp
    mySPDKApp.cpp
)

# --------------------------------------------------------------------------
# Include paths
# --------------------------------------------------------------------------
target_include_directories(mySPDKApp
    PRIVATE
        # SPDK public headers (needed for spdk_nvme_ctrlr_data, spdk_nvme_cmd, …)
        ${SPDK_INC_DIR}
        # mySPDKLib public header
        ${CMAKE_SOURCE_DIR}/mySPDKLib
)

# --------------------------------------------------------------------------
# Compile options
# --------------------------------------------------------------------------
target_compile_options(mySPDKApp PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -D_GNU_SOURCE
)

# --------------------------------------------------------------------------
# Link libraries
# --------------------------------------------------------------------------
target_link_libraries(mySPDKApp
    PRIVATE
        # Our own shared library (provides all SPDK/DPDK dependencies
        # transitively via its own RPATH and DT_NEEDED entries)
        mySPDKLib
)

# --------------------------------------------------------------------------
# Runtime library search paths (RPATH)
#
# The executable needs to find:
#   1. libmySPDKLib.so  — in the mySPDKLib sub-build directory
#   2. libspdk_*.so     — in the SPDK build tree
#   3. librte_*.so      — in the DPDK build tree
# --------------------------------------------------------------------------
set_target_properties(mySPDKApp PROPERTIES
    BUILD_RPATH
        "${CMAKE_BINARY_DIR}/mySPDKLib;${SPDK_LIB_DIR};${DPDK_LIB_DIR}"
    INSTALL_RPATH
        "${CMAKE_BINARY_DIR}/mySPDKLib;${SPDK_LIB_DIR};${DPDK_LIB_DIR}"
    INSTALL_RPATH_USE_LINK_PATH TRUE
)
