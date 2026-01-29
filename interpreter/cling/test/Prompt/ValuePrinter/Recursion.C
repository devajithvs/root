//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.

//------------------------------------------------------------------------------

// RUN: cat %s | %cling -Xclang -verify 2>&1 | FileCheck %s

.rawInput 1
#include <vector>
class json_test {
public:
    std::vector<json_test> data;
    json_test() = default;

    // auto-resize
    json_test& operator[](std::size_t i) {
        if (i >= data.size()) data.resize(i + 1);
        return data[i];
    }

    // Assignment to int (scalar)
    json_test& operator=(int) {
        data.clear();
        return *this;
    }

    // When empty (scalar), iterate over self
    auto begin() const { return data.empty() ? this : &data[0]; }
    auto end() const { return data.empty() ? this + 1 : &data[0] + data.size(); }
};
.rawInput 0

json_test j;
j[0] = 1
// CHECK: (json_test &) { <recursion detected> }

// expected-no-diagnostics
.q