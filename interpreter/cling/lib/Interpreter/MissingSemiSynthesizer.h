//--------------------------------------------------------------------*- C++ -*-
// CLING - the C++ LLVM-based InterpreterG :)
// author:  <your name>
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#ifndef CLING_MISSING_SEMI_SYNTHESIZER_H
#define CLING_MISSING_SEMI_SYNTHESIZER_H

#include "ASTTransformer.h"

namespace clang {
  class Decl;
  class Sema;
}

namespace cling {

  /// \brief Synthesizer that wraps top-level VarDecls into TopLevelStmtDecls
  /// with the "semicolon missing" bit set, so that cling treats them like
  /// expressions for value printing.
  class MissingSemiSynthesizer : public ASTTransformer {
  public:
    MissingSemiSynthesizer(clang::Sema* S);
    ~MissingSemiSynthesizer() override;

    Result Transform(clang::Decl*) override;
  };

} // namespace cling

#endif // CLING_MISSING_SEMI_SYNTHESIZER_H
