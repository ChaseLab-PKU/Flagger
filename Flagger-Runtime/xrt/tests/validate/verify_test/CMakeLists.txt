# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2019-2021 Xilinx, Inc. All rights reserved.
#
set(TESTNAME "validate.exe")

if (WIN32)
  set(OpenCL_INCLUDE_DIR ${OCL_ROOT}/include)
  find_library(OpenCL_LIBRARY
    NAMES OpenCL
    HINTS "${OCL_ROOT}/lib")
  include_directories(${OpenCL_INCLUDE_DIR})
else()
  find_package(OpenCL)
endif(WIN32)

include_directories(
  ../../../src/include/1_2
  ../../../src/runtime_src/core/include src/
  ../common/includes/xcl2
  ../common/includes/cmdparser
  ../common/includes/logger
  )

add_executable(${TESTNAME}
  ../common/includes/xcl2/xcl2.cpp
  ../common/includes/cmdparser/cmdlineparser.cpp
  ../common/includes/logger/logger.cpp src/host.cpp
  )

target_link_libraries(${TESTNAME} PRIVATE ${xrt_coreutil_LIBRARY} ${Boost_FILESYSTEM_LIBRARY} ${Boost_SYSTEM_LIBRARY})

if (NOT WIN32)
  target_link_libraries(${TESTNAME} PRIVATE ${uuid_LIBRARY} pthread ${OpenCL_LIBRARY} )
endif(NOT WIN32)

if (NOT DEFINED XRT_VALIDATE_DIR)
  set(XRT_VALIDATE_DIR "${CMAKE_CURRENT_BINARY_DIR}/")
endif()

install(TARGETS ${TESTNAME}
  RUNTIME DESTINATION ${XRT_VALIDATE_DIR}
  ) 

if (XRT_STATIC_BUILD)
  set(TESTNAME_STATIC "validate_static.exe")
  add_executable(${TESTNAME_STATIC}
    ../common/includes/xcl2/xcl2.cpp
    ../common/includes/cmdparser/cmdlineparser.cpp
    ../common/includes/logger/logger.cpp src/host.cpp
    )
  # Specify path directory with boost static libraries. The boost link dir
  # should be same as the one used to build XRT. In most case this is the
  # default /usr/lib, but if XRT was built with -with-static-boost, then
  # the libraries cannot be located without specified the link path.
  target_link_options(${TESTNAME_STATIC} PRIVATE "-static" "-L${Boost_LIBRARY_DIRS}")
  target_link_libraries(${TESTNAME_STATIC}
    PRIVATE
    # force linking of whole archive to enable static globals
    -Wl,--whole-archive xilinxopencl_static xrt++_static xrt_core_static xrt_coreutil_static -Wl,--no-whole-archive
    -Wl,--whole-archive rt pthread -Wl,--no-whole-archive
    )
  target_link_libraries(${TESTNAME_STATIC}
    PRIVATE
    xilinxopencl_static
    xrt++_static
    xrt_core_static
    xrt_coreutil_static
    )
  set_target_properties(${TESTNAME_STATIC} PROPERTIES INSTALL_RPATH "")
  install(TARGETS ${TESTNAME_STATIC} RUNTIME DESTINATION ${XRT_VALIDATE_DIR})
endif()
  
