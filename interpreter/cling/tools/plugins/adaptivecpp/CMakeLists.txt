include(ExternalProject)

set(HIPSYCL_NO_FIBERS ON)
set(WITH_OPENCL_BACKEND OFF)
set(WITH_LEVEL_ZERO_BACKEND OFF)

set(adaptivecpp_install_dir ${CMAKE_BINARY_DIR}/etc/cling/)
set(ADAPTIVE_CPP_INCLUDE_DIRS ${adaptivecpp_install_dir})
set(ADAPTIVE_CPP_HEADER_BUILD_PATH "${adaptivecpp_install_dir}/include/AdaptiveCpp")
set(ADAPTIVE_CPP_HEADER_INSTALL_PATH "${adaptivecpp_install_dir}/include/AdaptiveCpp")

set(_adaptivecpp_build_type ${CMAKE_CFG_INTDIR})
if(NOT _adaptivecpp_build_type STREQUAL "" AND NOT _adaptivecpp_build_type STREQUAL ".")
  set(EXTRA_BUILD_ARGS --config ${_adaptivecpp_build_type})
endif()

if(DEFINED ADAPTIVE_CPP_SOURCE_DIR)
  list(APPEND _adaptivecpp_settings SOURCE_DIR ${ADAPTIVE_CPP_SOURCE_DIR})
else()
  list(APPEND _adaptivecpp_settings
    GIT_REPOSITORY https://github.com/devajithvs/AdaptiveCpp.git
    GIT_TAG fbc807e495cc86e1a4fd982c309742a05ecac2de
  )
endif()

message(STATUS "Building Cling as part of LLVM")
  set(BACKEND_PACKAGE_STRING "${PACKAGE_STRING}")
  # Try finding the LLVMConfig.cmake if we build against prebuilt LLVM
  set(LLVM_CMAKE_PATH "${LLVM_BINARY_DIR}/lib${LLVM_LIBDIR_SUFFIX}/cmake/llvm")
  set(LLVMCONFIG_FILE "${LLVM_CMAKE_PATH}/LLVMConfig.cmake")
  if(EXISTS ${LLVMCONFIG_FILE})
    list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_PATH}")
    include(${LLVMCONFIG_FILE})
  endif()
  set(CLANG_CMAKE_DIR "${CMAKE_BINARY_DIR}/lib${LLVM_LIBDIR_SUFFIX}/cmake/clang")
  if (NOT CLANG_INCLUDE_DIRS)
    set (CLANG_INCLUDE_DIRS "${LLVM_MAIN_SRC_DIR}/tools/clang/include")
    if (NOT EXISTS ${CLANG_INCLUDE_DIRS})
      # Otherwise assume the monorepo layout.
      set (CLANG_INCLUDE_DIRS ${LLVM_MAIN_SRC_DIR}/../clang/include )
    endif()
    set (CLANG_INCLUDE_DIRS "${CLANG_INCLUDE_DIRS}"
      "${LLVM_BINARY_DIR}/tools/clang/include")
  endif()
  if (NOT LLVM_INCLUDE_DIRS)
    set (LLVM_INCLUDE_DIRS "${LLVM_MAIN_SRC_DIR}/include" "${LLVM_BINARY_DIR}/include")
  endif()
set(LLVM_DIR "${LLVM_BINARY_DIR}/lib/cmake/llvm")
message(WARNING "LLVM_DIR: ${LLVM_DIR}")

message(WARNING "DCLANG_INCLUDE_DIRS: ${CLANG_INCLUDE_DIRS}")


ExternalProject_Add(adaptivecpp
  UPDATE_COMMAND ""
  CMAKE_ARGS -G ${CMAKE_GENERATOR}
             -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
             -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
             -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
             -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
             -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
             -DCMAKE_INSTALL_PREFIX=${adaptivecpp_install_dir}
             -DHIPSYCL_NO_FIBERS=${HIPSYCL_NO_FIBERS}
             -DWITH_OPENCL_BACKEND=${WITH_OPENCL_BACKEND}
             -DWITH_LEVEL_ZERO_BACKEND=${WITH_LEVEL_ZERO_BACKEND}
             -DCUDAToolkit_LIBRARY_ROOT=/usr/lib/cuda
             -DCUDAToolkit_INCLUDE_DIRECTORIES=/usr/include/cuda_runtime.h
             -DCLANG_EXECUTABLE_PATH=${CLANG_EXECUTABLE_PATH}
             -DADAPTIVE_CPP_HEADER_BUILD_PATH=${ADAPTIVE_CPP_HEADER_BUILD_PATH}
             -DADAPTIVE_CPP_HEADER_INSTALL_PATH=${ADAPTIVE_CPP_HEADER_INSTALL_PATH}
             -DLLVM_DIR=${LLVM_DIR}
             -DCMAKE_PREFIX_PATH=${LLVM_DIR}
             -DLLVM_TOOLS_BINARY_DIR=${LLVM_TOOLS_BINARY_DIR}
             -DCLANG_INCLUDE_PATH=${CLANG_INCLUDE_DIRS}



  BUILD_COMMAND ${CMAKE_COMMAND} --build . ${EXTRA_BUILD_ARGS}
  INSTALL_COMMAND ${CMAKE_COMMAND} --build . ${EXTRA_BUILD_ARGS} --target install
  ${_adaptivecpp_settings}
  LOG_DOWNLOAD ON
  LOG_CONFIGURE ON
  LOG_BUILD ON
  LOG_INSTALL ON
  LOG_OUTPUT_ON_FAILURE ON
  DEPENDS clang llvm-link
)

# You can then IMPORT targets if needed (you'll need to know actual target names AdaptiveCpp defines)
