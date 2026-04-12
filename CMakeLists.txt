# ============================================================================
# spdkApp — Root CMake project
#
# Builds two targets:
#   libmySPDKLib.so   — shared library wrapping SPDK/DPDK NVMe APIs
#   mySPDKApp         — application exercising NVMe admin and I/O commands
#
# Prerequisites
# -------------
#   • SPDK built with shared libraries in /home/nitin/spdk/spdk/build/
#     (run `make` inside /home/nitin/spdk/spdk with CONFIG_SHARED=y)
#   • DPDK built as part of SPDK in /home/nitin/spdk/spdk/dpdk/build/
#   • fuse3, libnuma, libuuid, libpthread — usually present on Ubuntu/Fedora
#
# Build
# -----
#   mkdir -p build && cd build
#   cmake ..
#   make -j$(nproc)
#
# Run (requires VFIO or UIO binding of the target NVMe device, hugepages)
# -----
#   sudo ./mySPDKApp/mySPDKApp
# ============================================================================

cmake_minimum_required(VERSION 3.16)
project(spdkApp LANGUAGES CXX)

# --------------------------------------------------------------------------
# C++ standard
# --------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------
# Build type default
# --------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# --------------------------------------------------------------------------
# SPDK / DPDK paths
#
# Override SPDK_ROOT_DIR on the cmake command line if you keep SPDK
# elsewhere, e.g.:
#   cmake -DSPDK_ROOT_DIR=/opt/spdk ..
# --------------------------------------------------------------------------
set(SPDK_ROOT_DIR
    "/home/nitin/spdk/spdk"
    CACHE PATH "Root directory of the SPDK source / build tree")

set(SPDK_LIB_DIR  "${SPDK_ROOT_DIR}/build/lib"     CACHE PATH "SPDK shared-library directory")
set(SPDK_INC_DIR  "${SPDK_ROOT_DIR}/build/include"  CACHE PATH "SPDK public header directory")
set(DPDK_LIB_DIR  "${SPDK_ROOT_DIR}/dpdk/build/lib" CACHE PATH "DPDK shared-library directory")

# Sanity checks: make sure the key artefacts actually exist.
foreach(_check
        "${SPDK_LIB_DIR}/libspdk_nvme.so"
        "${SPDK_LIB_DIR}/libspdk_env_dpdk.so"
        "${DPDK_LIB_DIR}/librte_eal.so"
        "${SPDK_INC_DIR}/spdk/nvme.h")
    if(NOT EXISTS "${_check}")
        message(FATAL_ERROR
            "Required SPDK/DPDK artefact not found: ${_check}\n"
            "Please build SPDK shared libraries first:\n"
            "  cd ${SPDK_ROOT_DIR} && ./configure --with-shared && make -j$(nproc)")
    endif()
endforeach()

message(STATUS "SPDK root      : ${SPDK_ROOT_DIR}")
message(STATUS "SPDK lib dir   : ${SPDK_LIB_DIR}")
message(STATUS "SPDK inc dir   : ${SPDK_INC_DIR}")
message(STATUS "DPDK lib dir   : ${DPDK_LIB_DIR}")

# --------------------------------------------------------------------------
# Make SPDK/DPDK paths visible to sub-projects
# --------------------------------------------------------------------------
# (Variables set in the root scope are visible in subdirectories.)

# --------------------------------------------------------------------------
# Sub-projects (order matters: App depends on Lib)
# --------------------------------------------------------------------------
add_subdirectory(mySPDKLib)
add_subdirectory(mySPDKApp)

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
message(STATUS "")
message(STATUS "──────────────────────────────────────────────")
message(STATUS " spdkApp build summary")
message(STATUS "──────────────────────────────────────────────")
message(STATUS " Build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS " C++ standard  : C++${CMAKE_CXX_STANDARD}")
message(STATUS " Targets       : mySPDKLib  mySPDKApp")
message(STATUS "──────────────────────────────────────────────")
message(STATUS "")
