//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.

//------------------------------------------------------------------------------

// RUN: cat %s | %cling | FileCheck %s

#include <source_location>
std::source_location getsrcloc() {
#line 42 "CHECK_SRCLOC"
  return std::source_location::current();
}
getsrcloc()
// CHECK: (std::source_location) CHECK_SRCLOC:42:std::source_location getsrcloc()
