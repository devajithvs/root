//--------------------------------------------------------------------*- C++ -*-
// CLING - the C++ LLVM-based InterpreterG :)
// author: Roman Zulak
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#ifndef CLING_UTILS_CASTING_H
#define CLING_UTILS_CASTING_H

#include <cstdint>

namespace cling {
  namespace utils {

    ///\brief Legally cast a pointer to a function to void*.
    ///
    /// \param [in] funptr - The function to cast
    ///
    /// \return A void* of the functions address.
    ///
    template <typename T>
    void* FunctionToVoidPtr(T* funptr) {
        return reinterpret_cast<void*>(funptr);
    }

    ///\brief Legally cast a uintptr_t to a function.
    ///
    /// \param [in] ptr - The uintptr_t to cast
    ///
    /// \return The function's address.
    ///
    template <typename T>
    T UIntToFunctionPtr(uintptr_t ptr) {
        return reinterpret_cast<T>(ptr);
    }

    ///\brief Legally cast a void* to a function.
    ///
    /// \param [in] ptr - The void* to cast
    ///
    /// \return The function's address.
    ///
    template <typename T>
    T VoidToFunctionPtr(void* ptr) {
        return reinterpret_cast<T>(ptr);
    }

  } // namespace utils
} // namespace cling

#endif // CLING_UTILS_CASTING_H
