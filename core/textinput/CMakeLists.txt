# Copyright (C) 1995-2019, Rene Brun and Fons Rademakers.
# All rights reserved.
#
# For the licensing terms see $ROOTSYS/LICENSE.
# For the list of contributors see $ROOTSYS/README/CREDITS.

############################################################################
# CMakeLists.txt file for building ROOT core/textinout package
############################################################################

find_package(LLVM REQUIRED CONFIG NO_CMAKE_PACKAGE_REGISTRY)
include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
target_link_libraries(Core PRIVATE LLVMSupport LLVMLineEditor)

set_property(TARGET Core APPEND PROPERTY DICT_HEADERS Getline.h)

target_sources(Core PRIVATE
  src/Getline.cxx
)

target_include_directories(Core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>
  PRIVATE
    src
)

ROOT_INSTALL_HEADERS()
