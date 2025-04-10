# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2018-2021 Xilinx, Inc. All rights reserved.
#

PROJECT(XRT)
CMAKE_MINIMUM_REQUIRED(VERSION 3.5.0)

ENABLE_TESTING()

SET(PROJECT_DESCRIPTION "https://github.com/Xilinx/XRT")

# Include supporting CMake functions
include(CMake/unitTestSupport.cmake)

# Exported interface into XRT for include from implemenation
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime_src
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime_src/core/include
  ${CMAKE_BINARY_DIR}/gen
  )

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

message("-- Host system processor is ${CMAKE_HOST_SYSTEM_PROCESSOR}")
message("-- Target system processor is ${CMAKE_SYSTEM_PROCESSOR}")

set(XRT_NATIVE_BUILD "yes")
if (NOT ${CMAKE_SYSTEM_PROCESSOR} STREQUAL ${CMAKE_HOST_SYSTEM_PROCESSOR})
  set(XRT_NATIVE_BUILD "no")
endif()

if (DEFINED ENV{XRT_NATIVE_BUILD})
  set(XRT_NATIVE_BUILD $ENV{XRT_NATIVE_BUILD})
endif()

if (DEFINED ENV{XRT_CLANGTIDY_REVIEW})
  set(XRT_CLANGTIDY_REVIEW "yes")
  set(XRT_AIE_BUILD "yes")
  add_compile_options("-DXRT_ENABLE_AIE")
endif()

if (NOT CMAKE_BUILD_TYPE)
  set (CMAKE_BUILD_TYPE RelWithDebInfo)
endif (NOT CMAKE_BUILD_TYPE)

set (CMAKE_INSTALL_PREFIX "/opt/xilinx")
if (${XRT_NATIVE_BUILD} STREQUAL "no")
  set (CMAKE_INSTALL_PREFIX "/usr")
endif()

# Default component name for any install() command without the COMPONENT argument
# The default component is the xrt run-time component, if XRT_DEV_COMPONENT is
# set to something different then a development component will be created with
# link libraries and header which are then excluded from runtime component
set (CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "xrt")

# Enable development package by specifying development component name
# If XRT_DEV_COMPONENT is same DEFAULT_COMPONENT then only that package
# is created with both development and run-time content.
#set (XRT_DEV_COMPONENT "xrt-dev")
set (XRT_DEV_COMPONENT "xrt")

set (XRT_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# Version adjusted to 2.11 after u30 branch creation
set(XRT_VERSION_RELEASE 202120)
SET(XRT_VERSION_MAJOR 2)
SET(XRT_VERSION_MINOR 12)

if (DEFINED ENV{XRT_VERSION_PATCH})
  SET(XRT_VERSION_PATCH $ENV{XRT_VERSION_PATCH})
else(DEFINED $ENV{XRT_VERSION_PATCH})
  SET(XRT_VERSION_PATCH 0)
endif(DEFINED ENV{XRT_VERSION_PATCH})

#set(XRT_SOVERSION ${XRT_VERSION_MAJOR}.${XRT_VERSION_MINOR})
set(XRT_SOVERSION ${XRT_VERSION_MAJOR})
set(XRT_VERSION_STRING ${XRT_VERSION_MAJOR}.${XRT_VERSION_MINOR}.${XRT_VERSION_PATCH})

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/CMake/")

if (${XRT_NATIVE_BUILD} STREQUAL "yes")
  # Temporary native wrapper while Linux code is being ported to windows.
  # When completed the two build flows will once again be merged into one
  # common file
  if (WIN32)
    include(CMake/nativeWin.cmake)
  else()
    include(CMake/nativeLnx.cmake)
  endif()
  include(CMake/nativeTests.cmake)
else()
  include(CMake/embedded_system.cmake)
endif()
