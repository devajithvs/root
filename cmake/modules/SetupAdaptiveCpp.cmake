message(STATUS "Building AdaptiveCpp for SYCL support.")

include(FetchContent)

if (NOT DEFINED ADAPTIVE_CPP_SOURCE_DIR)
    FetchContent_Declare(
        AdaptiveCpp
        GIT_REPOSITORY https://github.com/devajithvs/AdaptiveCpp.git
        GIT_TAG 4d506b75d6b3e2cf9afb5de9dad759cce4fbe55c
    )
    FetchContent_GetProperties(AdaptiveCpp)
    if(NOT AdaptiveCpp_POPULATED)
        FetchContent_Populate(AdaptiveCpp)
    endif()
    set(ADAPTIVE_CPP_SOURCE_DIR "${adaptivecpp_SOURCE_DIR}")
    message(STATUS "Fetched AdaptiveCpp source to: ${ADAPTIVE_CPP_SOURCE_DIR}")
else()
    message(STATUS "ADAPTIVE_CPP_SOURCE_DIR already defined: ${ADAPTIVE_CPP_SOURCE_DIR}")
endif()

# FIXME: This is hardcoded
set(LLVM_BINARY_DIR ${CMAKE_BINARY_DIR}/interpreter/llvm-project/llvm)

# Standard path for LLVM external projects
set(ADAPTIVE_CPP_BINARY_DIR "${LLVM_BINARY_DIR}/tools/AdaptiveCpp")
message(STATUS "AdaptiveCpp will be built in: ${ADAPTIVE_CPP_TOOLS_DIR}")

list(APPEND CMAKE_PREFIX_PATH "${ADAPTIVE_CPP_BINARY_DIR}")
message(STATUS "Added ${ADAPTIVE_CPP_BINARY_DIR} to CMAKE_PREFIX_PATH.")

set(ADAPTIVE_CPP_ACPP_BIN "${ADAPTIVE_CPP_BINARY_DIR}/bin/acpp" CACHE FILEPATH "Path to the 'acpp' compiler executable." FORCE)
set(ADAPTIVE_CPP_CLANG_BIN "${LLVM_BINARY_DIR}/bin/clang++" CACHE FILEPATH "Path to the 'clang++' executable used by acpp." FORCE)

set(ADAPTIVECPP_INSTALL_CMAKE_DIR
  "lib/cmake/AdaptiveCpp" CACHE PATH "Install path for CMake config files")

# Set relative paths for install root in the following variables so that
# configure_package_config_file will generate paths relative whatever is
# the future install root
set(ADAPTIVECPP_INSTALL_COMPILER_DIR "${ADAPTIVE_CPP_BINARY_DIR}/bin")
set(ADAPTIVECPP_INSTALL_LAUNCHER_DIR "${ADAPTIVE_CPP_SOURCE_DIR}/cmake")
set(ADAPTIVECPP_INSTALL_LAUNCHER_RULE_DIR "${ADAPTIVE_CPP_SOURCE_DIR}/cmake")

# Make a config file to make this usable as a CMake Package
# Start by adding the version in a CMake understandable way
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${ADAPTIVE_CPP_SOURCE_DIR}/cmake/adaptivecpp-config.cmake.in
    ${ADAPTIVE_CPP_BINARY_DIR}/adaptivecpp-config.cmake
    INSTALL_DESTINATION ${ADAPTIVECPP_INSTALL_CMAKE_DIR}
    PATH_VARS
    ADAPTIVECPP_INSTALL_COMPILER_DIR
    ADAPTIVECPP_INSTALL_LAUNCHER_DIR
    ADAPTIVECPP_INSTALL_LAUNCHER_RULE_DIR
)
