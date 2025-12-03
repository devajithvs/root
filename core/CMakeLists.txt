# Copyright (C) 1995-2019, Rene Brun and Fons Rademakers.
# All rights reserved.
#
# For the licensing terms see $ROOTSYS/LICENSE.
# For the list of contributors see $ROOTSYS/README/CREDITS.

############################################################################
# CMakeLists.txt file for building ROOT (global) core package
############################################################################

include(SetROOTVersion)

set_source_files_properties(${CMAKE_BINARY_DIR}/ginclude/RConfigure.h
  PROPERTIES GENERATED TRUE)

add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/include/RConfigure.h
  COMMAND
    ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_BINARY_DIR}/ginclude/RConfigure.h
    ${CMAKE_BINARY_DIR}/include/RConfigure.h
  DEPENDS
    ${CMAKE_BINARY_DIR}/ginclude/RConfigure.h
)

add_custom_target(rconfigure ALL DEPENDS ${CMAKE_BINARY_DIR}/include/RConfigure.h)

ROOT_LINKER_LIBRARY(Core BUILTINS LZMA)

# This sets RUNPATH for libCore, which enables it to dlopen any ROOT library in its directory.
# If omitted, Core might try to load libraries from a ROOT that's installed in the system.
if(APPLE)
  set_property(TARGET Core APPEND PROPERTY BUILD_RPATH @loader_path)
elseif(NOT WIN32)
  set_property(TARGET Core APPEND PROPERTY BUILD_RPATH $ORIGIN)
endif()

generateHeader(Core
  ${CMAKE_SOURCE_DIR}/core/base/src/root-argparse.py
  ${CMAKE_BINARY_DIR}/ginclude/TApplicationCommandLineOptionsHelp.h
)

add_dependencies(Core CLING rconfigure)

target_link_libraries(Core
  PRIVATE
    ${CMAKE_DL_LIBS}
    ${CMAKE_THREAD_LIBS_INIT}
    ${ROOT_ATOMIC_LIBS}
)

if (CMAKE_SYSTEM_NAME MATCHES FreeBSD)
  target_link_libraries(Core PUBLIC util procstat)
endif()

target_include_directories(Core PUBLIC
   $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/ginclude>
)

# This is used by rootcling_stage1 and dictgen.
CHECK_CXX_COMPILER_FLAG("-fno-rtti" CXX_HAS_fno_rtti)

# Must be first because it defines ROOTStaticSanitizerConfig which needs
# to be linked into rootcling_stage1 (via ROOT_EXECUTABLE).
add_subdirectory(sanitizer)

add_subdirectory(rootcling_stage1)

add_subdirectory(base)
add_subdirectory(clib)
add_subdirectory(clingutils)
add_subdirectory(cont)
add_subdirectory(dictgen)
add_subdirectory(foundation)
add_subdirectory(gui)
add_subdirectory(imt)
add_subdirectory(meta)
add_subdirectory(metacling)
add_subdirectory(multiproc)
add_subdirectory(newdelete)
add_subdirectory(rint)
add_subdirectory(testsupport)
add_subdirectory(textinput)
add_subdirectory(thread)
add_subdirectory(zip)
add_subdirectory(lzma)
add_subdirectory(lz4)
add_subdirectory(zstd)

add_subdirectory(macosx)
add_subdirectory(unix)
add_subdirectory(winnt)

#-------------------------------------------------------------------------------

set(extra_cxxmodules_dependencies)
if(runtime_cxxmodules)
  # We load the builtin modulemap at rootcling bare-cling init.
  # FIXME: What happens when the <resource_dir>/include/* changes?
  #ROOT_GENERATE_CXXMODULE(_Builtin_stddef_max_align_t)
  #ROOT_GENERATE_CXXMODULE(_Builtin_intrinsics)
  set (libc)
  if (APPLE)
    set(libc Darwin)
  else()
    set(libc libc)
  endif()

  ROOT_GENERATE_CXXMODULE(${libc} "/usr/include")

  get_property(cxx_headers_location GLOBAL PROPERTY ROOT_CLING_CXX_HEADERS_LOCATION)

  # Assume the first component is where the public headers are.
  string(FIND ${cxx_headers_location} ":" idx)
  string(SUBSTRING ${cxx_headers_location} 0 ${idx} cxx_headers_location)

  ROOT_GENERATE_CXXMODULE(std ${cxx_headers_location} DEPENDENCIES ${libc})
  ROOT_GENERATE_CXXMODULE(Cling_Runtime ${CMAKE_BINARY_DIR}/etc/cling DEPENDENCIES std)
  ROOT_GENERATE_CXXMODULE(Cling_Runtime_Extra ${CMAKE_BINARY_DIR}/etc/cling DEPENDENCIES Cling_Runtime)

  if(cuda)
    ROOT_GENERATE_CXXMODULE(cuda ${CUDA_INCLUDE_DIRS})
  endif(cuda)

  ROOT_GENERATE_CXXMODULE(ROOT_Config ${CMAKE_BINARY_DIR}/include DEPENDENCIES Cling_Runtime_Extra)
  ROOT_GENERATE_CXXMODULE(ROOT_Rtypes ${CMAKE_BINARY_DIR}/include DEPENDENCIES ROOT_Config)
  ROOT_GENERATE_CXXMODULE(ROOT_Foundation_C ${CMAKE_BINARY_DIR}/include DEPENDENCIES ROOT_Rtypes)
  ROOT_GENERATE_CXXMODULE(ROOT_Foundation_Stage1_NoRTTI ${CMAKE_BINARY_DIR}/include DEPENDENCIES ROOT_Foundation_C)

  set(extra_cxxmodules_dependencies ${libc} std Cling_Runtime Cling_Runtime_Extra ROOT_Config ROOT_Rtypes ROOT_Foundation_Stage1_NoRTTI)
endif(runtime_cxxmodules)

if (libcxx AND NOT APPLE)
  # In case we use libcxx and glibc together there is a mismatch of the
  # signatures of functions in the header wchar.h. This macro tweaks the
  # header in rootcling resource directory to be compatible with the one from
  # libc++.
  target_compile_definitions(Core PRIVATE __CORRECT_ISO_CPP_WCHAR_H_PROTO)
endif()

if(MSVC)
  # Definitions of MATH Macros (required in MathCore) like M_PI are only
  # exposed on Windows after defining the _USE_MATH_DEFINES macro. By
  # specifying it as a property for Core, we ensure that the macros are
  # properly exposed when using Modules on Windows.
  target_compile_definitions(Core PRIVATE _USE_MATH_DEFINES)
endif()

# Inclusion of `complex.h` causes havoc: `complex` and `I` become CPP macros.
# Darwin's complex.h does not implement C11's __STDC_NO_COMPLEX__, use the
# header guard instead. This prevents inclusion of complex.h in Darwin.pcm.
# GCC <=5 has _COMPLEX_H but none of the others.
# __CLANG_STDATOMIC_H prevents inclusion of stdatomic in our Darwin.pcm: its
# macros cause conflics with boost.
target_compile_definitions(Core PRIVATE
  __STDC_NO_COMPLEX__
  __COMPLEX_H__
  _COMPLEX_H

  __CLANG_STDATOMIC_H
 )

if (runtime_cxxmodules)
  list(APPEND core_implicit_modules "-mSystemByproducts")
  # Force generation of _Builtin_intrinsics from Core.
  list(APPEND core_implicit_modules "-m" "_Builtin_intrinsics" "-mByproduct" "_Builtin_intrinsics")
  list(APPEND core_implicit_modules "-mByproduct" "ROOT_Foundation_Stage1_NoRTTI")
  list(APPEND core_implicit_modules "-mByproduct" "ROOT_Foundation_C")
  list(APPEND core_implicit_modules "-mByproduct" "ROOT_Rtypes")
endif(runtime_cxxmodules)

get_target_property(CORE_DICT_HEADERS Core DICT_HEADERS)

ROOT_GENERATE_DICTIONARY(G__Core
  ${CORE_DICT_HEADERS}
  STAGE1
  MODULE
    Core
  OPTIONS
    -writeEmptyRootPCM
    ${core_implicit_modules}
  LINKDEF
    base/inc/LinkDef.h
  DEPENDENCIES
    ${extra_cxxmodules_dependencies}
)
