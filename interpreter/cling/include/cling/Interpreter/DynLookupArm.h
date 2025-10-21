//===- DynLookupArm.h -------------------------------------------*- C++ -*-===//
//
// A tiny per-ASTContext switch for dynamic lookup. Avoids scanning the TU and
// does not depend on Transaction boundaries.
//
//===----------------------------------------------------------------------===//

#ifndef CLING_INTERPRETER_DYNLOOKUPARM_H
#define CLING_INTERPRETER_DYNLOOKUPARM_H

#include "llvm/ADT/DenseMap.h"

namespace clang { class ASTContext; class Decl; }

namespace cling {

class DynLookupArm {
public:
  // Marks the current compilation "armed" for dynamic lookup.
  static void arm(const clang::ASTContext& Ctx, const clang::Decl* Marker) {
    State& S = map()[&Ctx];
    S.Armed = true;
    S.Marker = Marker;
  }

  // Clears the armed flag explicitly (e.g. at chunk boundaries).
  static void disarm(const clang::ASTContext& Ctx) {
    auto It = map().find(&Ctx);
    if (It != map().end()) {
      It->second.Armed = false;
      It->second.Marker = nullptr;
    }
  }

  static bool isArmed(const clang::ASTContext& Ctx) {
    auto It = map().find(&Ctx);
    return It != map().end() && It->second.Armed;
  }

  // Optional: access to the marker if someone wants it (debug/logging).
  static const clang::Decl* marker(const clang::ASTContext& Ctx) {
    auto It = map().find(&Ctx);
    return (It != map().end()) ? It->second.Marker : nullptr;
  }

private:
  struct State {
    bool Armed = false;
    const clang::Decl* Marker = nullptr; // TU-local synthetic decl we created
  };
  static llvm::DenseMap<const clang::ASTContext*, State>& map() {
    static llvm::DenseMap<const clang::ASTContext*, State> M;
    return M;
  }
};

} // namespace cling

#endif // CLING_INTERPRETER_DYNLOOKUPARM_H
