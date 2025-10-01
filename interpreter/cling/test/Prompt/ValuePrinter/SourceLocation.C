//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.

//------------------------------------------------------------------------------
// This test printing of std::source_location, which is a C++20 feature.
// Running it with an older language standard is meaningless and brittle to test
// Explicitly enforce -std=c++20 here.
// RUN: cat %s | %cling -std=c++20 | FileCheck %s

#include <iostream>

#include <source_location>
std::source_location getsrcloc() {
#line 42 "CHECK_SRCLOC"
  return std::source_location::current();
}
getsrcloc()
// CHECK: (std::source_location)
// CHECK: CHECK_SRCLOC:42:std::source_location getsrcloc()
