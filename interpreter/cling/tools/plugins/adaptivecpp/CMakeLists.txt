#-------------------------------------------------------------------------------
# CLING - the C++ LLVM-based InterpreterG :)
#
# This file is dual-licensed: you can choose to license it under the University
# of Illinois Open Source License or the GNU Lesser General Public License. See
# LICENSE.TXT for details.
#-------------------------------------------------------------------------------

include(ExternalProject)

set(adaptivecpp_install_dir ${CMAKE_BINARY_DIR}/etc/cling/)
# Specify include dirs for adaptivecpp
set(ADAPTIVECPP_INCLUDE_DIRS ${adaptivecpp_install_dir})
# AdaptiveCpp Libraries
set(_adaptivecpp_build_type ${CMAKE_CFG_INTDIR})
if(MSVC)
  if (winrtdebug)
    set(_adaptivecpp_build_type Debug)
  else()
    set(_adaptivecpp_build_type Release)
  endif()
endif(MSVC)
set(_ADAPTIVECPP_LIBRARY_PATH ${CMAKE_CURRENT_BINARY_DIR}/adaptivecpp-prefix/src/adaptivecpp-build/${_adaptivecpp_build_type}/lib${LLVM_LIBDIR_SUFFIX})

if(APPLE)
  set(_adaptivecpp_extra_cmake_args -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT})
endif()

if (CMAKE_CXX_STANDARD)
  list(APPEND _adaptivecpp_extra_cmake_args -DCMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD})
endif(CMAKE_CXX_STANDARD)

if (Clang_DIR)
  list(APPEND _adaptivecpp_extra_cmake_args -DClang_DIR=${Clang_DIR} -DClang_CONFIG_EXTRA_PATH_HINTS=${Clang_Config_ExtraPathHints})
endif(Clang_DIR)

if (LLVM_FORCE_USE_OLD_TOOLCHAIN)
  list(APPEND _adaptivecpp_extra_cmake_args -DLLVM_FORCE_USE_OLD_TOOLCHAIN=${LLVM_FORCE_USE_OLD_TOOLCHAIN})
endif(LLVM_FORCE_USE_OLD_TOOLCHAIN)

if (CLING_INCLUDE_TESTS)
  list(APPEND _adaptivecpp_extra_cmake_args -DLLVM_EXTERNAL_LIT=${LLVM_MAIN_SRC_DIR}/utils/lit/lit.py)
else()
  list(APPEND _adaptivecpp_extra_cmake_args -DADAPTIVECPP_BUILD_STATIC_ONLY=ON)
endif()

# Wrap download, configure and build steps in a script to log output
set(_adaptivecpp_cmake_logging_settings
  LOG_DOWNLOAD ON
  LOG_CONFIGURE ON
  LOG_BUILD ON
  LOG_INSTALL ON
)

list(APPEND _adaptivecpp_cmake_logging_settings LOG_OUTPUT_ON_FAILURE ON)

ExternalProject_Add(
  adaptivecpp
  GIT_REPOSITORY https://github.com/AdaptiveCpp/AdaptiveCpp.git
  GIT_TAG v24.02.0
  UPDATE_COMMAND ""
  CMAKE_ARGS -G ${CMAKE_GENERATOR}
             -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
             -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
             -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
             -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
             -DCMAKE_INSTALL_PREFIX=${adaptivecpp_install_dir}/plugins
             -DLLVM_DIR=${LLVM_BINARY_DIR}
             -DCLANG_INCLUDE_DIRS=${CLANG_INCLUDE_DIRS}
             ${_adaptivecpp_extra_cmake_args}
  BUILD_COMMAND ${CMAKE_COMMAND} --build . --config ${_adaptivecpp_build_type}
  INSTALL_COMMAND ${CMAKE_COMMAND} --build . --config ${_adaptivecpp_build_type} --target install
  ${_adaptivecpp_cmake_logging_settings}
  # Always run the build command so that incremental builds are correct.
  BUILD_ALWAYS 1
  # We need the target clangBasic to be built before building adaptivecpp. However, we
  # support building prebuilt clang and adding clangBasic breaks this case.
  # Delegate the dependency resolution to the clingInterpreter target (which
  # will always depend on clangBasic).
  DEPENDS LLVM

  )

  install(CODE "execute_process\(COMMAND \${CMAKE_COMMAND} -DCMAKE_INSTALL_PREFIX=\${CMAKE_INSTALL_PREFIX} -P ${BINARY_DIR}/cmake_install.cmake \)"
    COMPONENT adaptivecpp)

