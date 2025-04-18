# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2019-2021 Xilinx, Inc. All rights reserved.
#
CMAKE_MINIMUM_REQUIRED(VERSION 3.0.0)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(MODE hw)

if (DEFINED ENV{XCL_EMULATION_MODE})
  set(MODE $ENV{XCL_EMULATION_MODE})
  string(REPLACE "_" "" XCL_EMU_SUFFIX ${MODE})
endif()

if (NOT DEFINED ${INSTALL_DIR})
  set(INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/${CMAKE_SYSTEM_NAME}/${CMAKE_BUILD_TYPE}/${MODE}")
endif()

add_subdirectory(verify_test)
add_subdirectory(bandwidth_test)
add_subdirectory(hostmemory_test)

# Special iops performance test cases
add_subdirectory(xcl_iops_test)
add_subdirectory(xcl_vcu_test)
add_subdirectory(xrt_iops_test)
