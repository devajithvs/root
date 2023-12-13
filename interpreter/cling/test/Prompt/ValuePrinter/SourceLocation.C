//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.

//------------------------------------------------------------------------------

// RUN: cat %s | %cling | FileCheck %s

#if __cplusplus >= 202002L
#include <source_location>
std::source_location::current()
// CHECK: (std::source_location) ROOT_prompt_0:2:__cling_Un1Qu30
#endif
