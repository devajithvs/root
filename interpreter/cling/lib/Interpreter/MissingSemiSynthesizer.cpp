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

      // Suppress the "expected ';' after top level declarator" error
        auto& Diags = Ctx.getDiagnostics();
        if (Diags.hasErrorOccurred()) {
        // Reset counts, but keep other state intact
        Diags.Reset(false /*soft - only counts, not mappings*/);
        }

      // Create a DeclRefExpr to reference the variable
      Expr* DRE = DeclRefExpr::Create(Ctx,
                                      NestedNameSpecifierLoc(),
                                      SourceLocation(),
                                      VD,
                                      false,
                                      VD->getLocation(),
                                      VD->getType(),
                                      VK_LValue);

        auto* TLSD = TopLevelStmtDecl::Create(Ctx, DRE);
        // Pretend the user omitted the semicolon
        TLSD->setSemiMissing(true);
        // VD->setIsUsed();
      return Result(TLSD, true);
    }
    return Result(D, true);
  }

} // namespace cling
