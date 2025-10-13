#include "MissingSemiSynthesizer.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/Sema/Sema.h"

using namespace clang;

namespace cling {

  MissingSemiSynthesizer::MissingSemiSynthesizer(clang::Sema* S)
    : ASTTransformer(S) {}

  MissingSemiSynthesizer::~MissingSemiSynthesizer() {}

  ASTTransformer::Result MissingSemiSynthesizer::Transform(Decl* D) {
    // Only wrap top-level variable declarations
    if (auto* VD = llvm::dyn_cast<VarDecl>(D)) {
      ASTContext& Ctx = m_Sema->getASTContext();

      // Create a DeclRefExpr to reference the variable
      Expr* DRE = VD->getInit();

      DRE->dump();

        auto* TLSD = TopLevelStmtDecl::Create(Ctx, DRE);
        llvm::errs() << "Dumpting TLSD\n";
        TLSD->dump();
        // Pretend the user omitted the semicolon
        TLSD->setSemiMissing(true);
        VD->setIsUsed();
      return Result(TLSD, true);
    }
    return Result(D, true);
  }

} // namespace cling
