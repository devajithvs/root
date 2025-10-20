//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// author:  Vassil Vassilev <vasil.georgiev.vasilev@cern.ch>
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#include "ValueExtractionSynthesizer.h"

#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/Transaction.h"
#include "cling/Interpreter/Value.h"
#include "cling/Interpreter/Visibility.h"
#include "cling/Utils/AST.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclGroup.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaDiagnostic.h"

using namespace clang;

#include "llvm/Support/raw_ostream.h"
#include "clang/AST/PrettyPrinter.h"

static std::string typeToString(const clang::ASTContext& Ctx, clang::QualType QT) {
  clang::PrintingPolicy PP(Ctx.getPrintingPolicy());
  PP.SuppressUnwrittenScope = true;
  PP.ConstantsAsWritten = true;
  std::string S; llvm::raw_string_ostream OS(S);
  QT.print(OS, PP); OS.flush(); return S;
}

static std::string exprToString(const clang::ASTContext& Ctx, const clang::Expr* E) {
  if (!E) return "<null-expr>";
  clang::PrintingPolicy PP(Ctx.getPrintingPolicy());
  PP.SuppressUnwrittenScope = true;
  PP.ConstantsAsWritten = true;
  std::string S; llvm::raw_string_ostream OS(S);
  E->printPretty(OS, /*Helper*/nullptr, PP); OS.flush();
  if (S.empty()) S = std::string("<") + E->getStmtClassName() + ">";
  return S;
}

static const char* valueKindStr(::clang::ExprValueKind VK) {
  using VK_t = ::clang::ExprValueKind;
  switch (VK) {
    case VK_t::VK_PRValue: return "prvalue";
    case VK_t::VK_LValue: return "lvalue";
    case VK_t::VK_XValue: return "xvalue";
  }
  return "?";
}

static const char* boolStr(bool b){ return b ? "true" : "false"; }


namespace cling {
  ValueExtractionSynthesizer::ValueExtractionSynthesizer(clang::Sema* S, Interpreter* m_Interpreter,
                                                         bool isChildInterpreter)
    : WrapperTransformer(S), m_Context(&S->getASTContext()), m_gClingVD(0),
      m_UnresolvedNoAlloc(0), m_UnresolvedWithAlloc(0),
      m_UnresolvedCopyArray(0), m_isChildInterpreter(isChildInterpreter), m_Interpreter(m_Interpreter) { }

  // pin the vtable here.
  ValueExtractionSynthesizer::~ValueExtractionSynthesizer() { }

  namespace {
    class ReturnStmtCollector : public StmtVisitor<ReturnStmtCollector> {
    private:
      llvm::SmallVectorImpl<Stmt**>& m_Stmts;
    public:
      ReturnStmtCollector(llvm::SmallVectorImpl<Stmt**>& S)
        : m_Stmts(S) {}

      void VisitStmt(Stmt* S) {
        for(Stmt::child_iterator I = S->child_begin(), E = S->child_end();
            I != E; ++I) {
          if (!*I)
            continue;
          if (isa<LambdaExpr>(*I))
            continue;
          Visit(*I);
          if (isa<ReturnStmt>(*I))
            m_Stmts.push_back(&*I);
        }
      }
    };
  }

  ASTTransformer::Result ValueExtractionSynthesizer::Transform(clang::Decl* D) {
    const CompilationOptions& CO = getCompilationOpts();
    // If we do not evaluate the result, or printing out the result return.
    if (!(CO.ResultEvaluation || CO.ValuePrinting))
      return Result(D, true);

    FunctionDecl* FD = cast<FunctionDecl>(D);
    assert(utils::Analyze::IsWrapper(FD) && "Expected wrapper");

    int foundAtPos = -1;
    Expr* lastExpr = utils::Analyze::GetOrCreateLastExpr(FD, &foundAtPos,
                                                         /*omitDS*/false,
                                                         m_Sema);
    if (foundAtPos < 0)
      return Result(D, true);

    typedef llvm::SmallVector<Stmt**, 4> StmtIters;
    StmtIters returnStmts;
    ReturnStmtCollector collector(returnStmts);
    CompoundStmt* CS = cast<CompoundStmt>(FD->getBody());
    collector.VisitStmt(CS);

    if (isa<Expr>(*(CS->body_begin() + foundAtPos)))
      returnStmts.push_back(CS->body_begin() + foundAtPos);

    // We want to support cases such as:
    // gCling->evaluate("if() return 'A' else return 12", V), that puts in V,
    // either A or 12.
    // In this case the void wrapper is compiled with the stmts returning
    // values. Sema would cast them to void, but the code will still be
    // executed. For example:
    // int g(); void f () { return g(); } will still call g().
    //
    for (StmtIters::iterator I = returnStmts.begin(), E = returnStmts.end();
         I != E; ++I) {
      ReturnStmt* RS = dyn_cast<ReturnStmt>(**I);
      if (RS) {
        // When we are handling a return stmt, the last expression must be the
        // return stmt value. Ignore the calculation of the lastStmt because it
        // might be wrong, in cases where the return is not in the end of the
        // function.
        lastExpr = RS->getRetValue();
        if (lastExpr) {
          assert (lastExpr->getType()->isVoidType() && "Must be void type.");
          // Any return statement will have been "healed" by Sema
          // to correspond to the original void return type of the
          // wrapper, using a ImplicitCastExpr 'void' <ToVoid>.
          // Remove that.
          if (ImplicitCastExpr* VoidCast
              = dyn_cast<ImplicitCastExpr>(lastExpr)) {
            lastExpr = VoidCast->getSubExpr();
          }
        }
        // if no value assume void
        else {
          // We can't PushDeclContext, because we don't have scope.
          Sema::ContextRAII pushedDC(*m_Sema, FD);
          RS->setRetValue(SynthesizeSVRInit(0));
        }

      }
      else
        lastExpr = cast<Expr>(**I);

      if (lastExpr) {
        QualType lastExprTy = lastExpr->getType();
        // May happen on auto types which resolve to dependent.
        if (lastExprTy->isDependentType())
          continue;
        // Set up lastExpr properly.
        // Change the void function's return type
        // We can't PushDeclContext, because we don't have scope.
        Sema::ContextRAII pushedDC(*m_Sema, FD);

        if (lastExprTy->isFunctionType()) {
          // A return type of function needs to be converted to
          // pointer to function.
          lastExprTy = m_Context->getPointerType(lastExprTy);
          lastExpr = m_Sema->ImpCastExprToType(lastExpr, lastExprTy,
                                               CK_FunctionToPointerDecay,
                                               VK_PRValue).get();
        }

        //
        // Here we don't want to depend on the JIT runFunction, because of its
        // limitations, when it comes to return value handling. There it is
        // not clear who provides the storage and who cleans it up in a
        // platform independent way.
        //
        // Depending on the type we need to synthesize a call to cling:
        // 0) void : set the value's type to void;
        // 1) enum, integral, float, double, referece, pointer types :
        //      call to cling::internal::setValueNoAlloc(...);
        // 2) object type (alloc on the stack) :
        //      cling::internal::setValueWithAlloc
        //   2.1) constant arrays:
        //          call to cling::runtime::internal::copyArray(...)
        //
        // We need to synthesize later:
        // Wrapper has signature: void w(cling::Value SVR)
        // case 1):
        //   setValueNoAlloc(gCling, &SVR, lastExprTy, lastExpr())
        // case 2):
        //   new (setValueWithAlloc(gCling, &SVR, lastExprTy)) (lastExpr)
        // case 2.1):
        //   copyArray(src, placement, size)

        Expr* SVRInit = SynthesizeSVRInit(lastExpr);
        // if we had return stmt update to execute the SVR init, even if the
        // wrapper returns void.
        if (SVRInit) {
          if (RS) {
            if (ImplicitCastExpr* VoidCast
                = dyn_cast<ImplicitCastExpr>(RS->getRetValue()))
              VoidCast->setSubExpr(SVRInit);
          } else
            **I = SVRInit;
        } else {
          // FIXME: Do this atomically or something so that AST context will not
          // contain Expr(s) that are unused for the rest of it's life.
          return Result(D, false);
        }
      }
    }
    return Result(D, true);
  }

// Helper function for the SynthesizeSVRInit
namespace {
  static bool availableCopyConstructor(QualType QT, clang::Sema* S) {
    // Check the the existance of the copy constructor the tha placement new will use.
    if (CXXRecordDecl* RD = QT->getAsCXXRecordDecl()) {
      // If it has a trivial copy constructor it is accessible and it is callable.
      if(RD->hasTrivialCopyConstructor()) return true;
      // Lookup the copy canstructor and check its accessiblity.
      if (CXXConstructorDecl* CD = S->LookupCopyingConstructor(RD, QT.getCVRQualifiers())) {
        if (!CD->isDeleted() && CD ->getAccess() == clang::AccessSpecifier::AS_public) {
          return true;
        }
      }
      return false;
    }
    return true;
  }
}


Expr* ValueExtractionSynthesizer::SynthesizeSVRInit(Expr* E) {
  ::llvm::errs() << "[SVR] enter SynthesizeSVRInit E=" << (const void*)E << "\n";
    llvm::errs() << "[SVR-fn] E(before) : " << exprToString(*m_Context, E) << "\n";

  if (!m_gClingVD && !FindAndCacheRuntimeDecls(E)) {
    ::llvm::errs() << "[SVR] runtime decls missing => return nullptr\n";
    return nullptr;
  }

  // Current context
  FunctionDecl* FD = cast<FunctionDecl>(m_Sema->CurContext);
  ::llvm::errs() << "[SVR] CurContext FD=" << (const void*)FD
               << " name=" << (FD->getIdentifier() ? FD->getName() : "<anon>") << "\n";

  ExprWithCleanups* Cleanups = nullptr;
  if (E && isa<ExprWithCleanups>(E)) {
    Cleanups = cast<ExprWithCleanups>(E);
    ::llvm::errs() << "[SVR] E is ExprWithCleanups: " << (const void*)Cleanups
                 << " sub=" << (const void*)Cleanups->getSubExpr() << "\n";
    E = Cleanups->getSubExpr();
  }

  SourceLocation locStart = (E) ? E->getBeginLoc() : FD->getBeginLoc();
  SourceLocation locEnd   = (E) ? E->getEndLoc()   : FD->getEndLoc();
  QualType ETy            = (E) ? E->getType()     : m_Context->VoidTy;
  QualType desugaredTy    = ETy.getDesugaredType(*m_Context);

  if (E) {
    ::llvm::errs() << "[SVR] E(before) = " << exprToString(*m_Context, E) << "\n";
    ::llvm::errs() << "[SVR] E VK=" << valueKindStr(E->getValueKind())
                 << " ETy=" << typeToString(*m_Context, ETy)
                 << " desugared=" << typeToString(*m_Context, desugaredTy) << "\n";
  } else {
    ::llvm::errs() << "[SVR] no E (void context)\n";
  }

  // If record lvalue, use reference type
  if (E && desugaredTy->isRecordType() && E->getValueKind() == VK_LValue) {
    desugaredTy = m_Context->getLValueReferenceType(desugaredTy);
    ETy         = m_Context->getLValueReferenceType(ETy);
    ::llvm::errs() << "[SVR] record lvalue -> make reference; "
                 << "ETy=" << typeToString(*m_Context, ETy)
                 << " desugared=" << typeToString(*m_Context, desugaredTy) << "\n";
  }

  // Params
  auto *ThisInterp = utils::Synthesize::CStyleCastPtrExpr(
      m_Sema, m_Context->VoidPtrTy, (uintptr_t)m_Interpreter);
  auto *OutValue = utils::Synthesize::CStyleCastPtrExpr(
      m_Sema, m_Context->VoidPtrTy, (uintptr_t)&(m_Interpreter->LastValue));
  Expr* ETyVP = utils::Synthesize::CStyleCastPtrExpr(
      m_Sema, m_Context->VoidPtrTy, (uintptr_t)ETy.getAsOpaquePtr());

  ::llvm::errs() << "[SVR] Args: ThisInterp=" << (const void*)ThisInterp
               << " OutValue=" << (const void*)OutValue
               << " ETyVP=" << (const void*)ETyVP << "\n";

  ::llvm::SmallVector<Expr*, 5> CallArgs;
  CallArgs.push_back(ThisInterp);
  CallArgs.push_back(OutValue);
  CallArgs.push_back(ETyVP);

  ExprResult Call;
  SourceLocation noLoc = locStart;

  // ---- Branch 1: void -------------------------------------------------------
  if (desugaredTy->isVoidType()) {
    ::llvm::errs() << "[SVR] branch: void result\n";
    QualType vpQT = m_Context->VoidPtrTy;
    QualType vQT  = m_Context->VoidTy;
    Expr* vpQTVP  = utils::Synthesize::CStyleCastPtrExpr(m_Sema, vpQT,
                        (uintptr_t)vQT.getAsOpaquePtr());
    CallArgs[2]   = vpQTVP;

    Call = m_Sema->ActOnCallExpr(/*Scope*/nullptr, m_UnresolvedNoAlloc,
                                 locStart, CallArgs, locEnd);
    if (Call.isInvalid())
      ::llvm::errs() << "[SVR] m_UnresolvedNoAlloc(Call) invalid\n";

    if (E) {
      ::llvm::errs() << "[SVR] comma with original E: " << exprToString(*m_Context, E) << "\n";
      Call = m_Sema->CreateBuiltinBinOp(locStart, BO_Comma, Call.get(), E);
      if (Call.isInvalid())
        ::llvm::errs() << "[SVR] comma operator creation invalid\n";
    }
  }
  // ---- Branch 2: object / array / member-ptr -------------------------------
  else if (desugaredTy->isRecordType() || desugaredTy->isConstantArrayType()
           || desugaredTy->isMemberPointerType()) {

    ::llvm::errs() << "[SVR] branch: object/array/memberptr "
                 << typeToString(*m_Context, desugaredTy) << "\n";

    if (!desugaredTy->isMemberPointerType()) {
      bool hasCC = availableCopyConstructor(desugaredTy, m_Sema);
      ::llvm::errs() << "[SVR] availableCopyConstructor=" << boolStr(hasCC) << "\n";
      if (!hasCC) {
        ::llvm::errs() << "[SVR] no copy ctor => return original E\n";
        return E;
      }
    }

    Call = m_Sema->ActOnCallExpr(/*Scope*/nullptr, m_UnresolvedWithAlloc,
                                 locStart, CallArgs, locEnd);
    if (Call.isInvalid())
      ::llvm::errs() << "[SVR] m_UnresolvedWithAlloc(Call) invalid\n";

    Expr* placement = Call.get();
    ::llvm::errs() << "[SVR] placement expr: " << exprToString(*m_Context, placement) << "\n";

    if (const ConstantArrayType* constArray
          = dyn_cast<ConstantArrayType>(desugaredTy.getTypePtr())) {

      ::llvm::errs() << "[SVR] sub-branch: constant array\n";
      CallArgs.clear();

      QualType baseElementType = m_Context->getBaseElementType(desugaredTy);
      TypeSourceInfo* TSI
        = m_Context->getTrivialTypeSourceInfo(m_Context->getPointerType(baseElementType), noLoc);
      Expr* srcPointer = m_Sema->BuildCStyleCastExpr(noLoc, TSI, noLoc, E).get();
      ::llvm::errs() << "[SVR] srcPointer: " << exprToString(*m_Context, srcPointer) << "\n";

      CallArgs.push_back(srcPointer);
      CallArgs.push_back(placement);

      size_t arrSize = m_Context->getConstantArrayElementCount(constArray);
      Expr* arrSizeExpr = utils::Synthesize::IntegerLiteralExpr(*m_Context, arrSize);
      CallArgs.push_back(arrSizeExpr);
      ::llvm::errs() << "[SVR] array size=" << arrSize << "\n";

      Call = m_Sema->ActOnCallExpr(/*Scope*/nullptr, m_UnresolvedCopyArray,
                                   locStart, CallArgs, locEnd);
      if (Call.isInvalid())
        ::llvm::errs() << "[SVR] m_UnresolvedCopyArray(Call) invalid\n";
    } else {
      if (!E || !E->getSourceRange().isValid()) {
        ::llvm::errs() << "[SVR] invalid source range for E -> cannot BuildCXXNew; return E\n";
        return E;
      }

      TypeSourceInfo* ETSI = m_Context->getTrivialTypeSourceInfo(ETy, noLoc);
      ::llvm::errs() << "[SVR] BuildCXXNew: allocType=" << typeToString(*m_Context, ETy)
                   << " init=" << exprToString(*m_Context, E) << "\n";

      Call = m_Sema->BuildCXXNew(E->getSourceRange(),
                                 /*useGlobal*/ true,
                                 /*lParen*/ noLoc,
                                 MultiExprArg(placement),
                                 /*rParen*/ noLoc,
                                 /*TypeIdParens*/ SourceRange(),
                                 /*allocType*/ ETSI->getType(),
                                 /*allocTypeInfo*/ ETSI,
                                 /*arraySize*/ {},
                                 /*directInitRange*/ E->getSourceRange(),
                                 /*initializer*/ E);
      if (Call.isInvalid()) {
        ::llvm::errs() << "[SVR] BuildCXXNew invalid; diagnose and return expr\n";
        m_Sema->Diag(E->getBeginLoc(), diag::err_undeclared_var_use) << "operator new";
        return Call.get();
      }

      Call = m_Sema->ActOnFinishFullExpr(Call.get(), /*DiscardedValue*/ false);
      if (Call.isInvalid())
        ::llvm::errs() << "[SVR] ActOnFinishFullExpr invalid\n";
    }
  }
  // ---- Branch 3: scalars / refs / ptrs / floats ----------------------------
  else {
    ::llvm::errs() << "[SVR] branch: scalar/ref/ptr/floating; "
                 << "desugared=" << typeToString(*m_Context, desugaredTy) << "\n";

    const size_t nArgs = CallArgs.size();

    if (desugaredTy->isIntegralOrEnumerationType()) {
      ::llvm::errs() << "[SVR] sub-branch: integral/enumeration\n";
      QualType UInt64Ty = m_Context->UnsignedLongLongTy;
      TypeSourceInfo* TSI = m_Context->getTrivialTypeSourceInfo(UInt64Ty, noLoc);
      Expr* castedE = m_Sema->BuildCStyleCastExpr(noLoc, TSI, noLoc, E).get();
      ::llvm::errs() << "[SVR] castedE: " << exprToString(*m_Context, castedE) << "\n";
      CallArgs.push_back(castedE);
    }
    else if (desugaredTy->isReferenceType()) {
      ::llvm::errs() << "[SVR] sub-branch: reference -> &E\n";
      Expr* AddrOfE  = m_Sema->CreateBuiltinUnaryOp(noLoc, UO_AddrOf, E).get();
      ::llvm::errs() << "[SVR] &E: " << exprToString(*m_Context, AddrOfE) << "\n";
      CallArgs.push_back(AddrOfE);
    }
    else if (desugaredTy->isAnyPointerType()) {
      ::llvm::errs() << "[SVR] sub-branch: pointer -> cast to void*\n";
      QualType VoidPtrTy = m_Context->VoidPtrTy;
      TypeSourceInfo* TSI
        = m_Context->getTrivialTypeSourceInfo(VoidPtrTy, noLoc);
      Expr* castedE
        = m_Sema->BuildCStyleCastExpr(noLoc, TSI, noLoc, E).get();
      ::llvm::errs() << "[SVR] castedE: " << exprToString(*m_Context, castedE) << "\n";
      CallArgs.push_back(castedE);
    }
    else if (desugaredTy->isNullPtrType()) {
      ::llvm::errs() << "[SVR] sub-branch: nullptr\n";
      CallArgs.push_back(E);
    }
    else if (desugaredTy->isFloatingType()) {
      ::llvm::errs() << "[SVR] sub-branch: floating\n";
      CallArgs.push_back(E);
    }

    if (CallArgs.size() > nArgs) {
      ::llvm::errs() << "[SVR] calling m_UnresolvedNoAlloc with "
                   << (CallArgs.size() - nArgs) << " value arg(s)\n";
      Call = m_Sema->ActOnCallExpr(/*Scope*/nullptr, m_UnresolvedNoAlloc,
                                   locStart, CallArgs, locEnd);
      if (Call.isInvalid())
        ::llvm::errs() << "[SVR] m_UnresolvedNoAlloc(Call) invalid\n";
    } else {
      ::llvm::errs() << "[SVR] unsupported unknown-any: "
                   << typeToString(*m_Context, desugaredTy) << "\n";
      m_Sema->Diag(locStart, diag::err_unsupported_unknown_any_decl)
        << utils::TypeName::GetFullyQualifiedName(desugaredTy, *m_Context)
        << SourceRange(locStart, locEnd);
    }
  }

  if (Call.isInvalid()) {
    ::llvm::errs() << "[SVR] Call is invalid at exit; returning nullptr\n";
    return nullptr;
  }

  // Extend cleanups if needed
  if (Cleanups) {
    ::llvm::errs() << "[SVR] wrap in ExprWithCleanups\n";
    Cleanups->setSubExpr(Call.get());
    Cleanups->setValueKind(Call.get()->getValueKind());
    Cleanups->setType(Call.get()->getType());
    ::llvm::errs() << "[SVR] result(with cleanups): "
                 << exprToString(*m_Context, Cleanups) << "\n";
    return Cleanups;
  }

  ::llvm::errs() << "[SVR] result: " << exprToString(*m_Context, Call.get())
               << " : " << typeToString(*m_Context, Call.get()->getType()) << "\n";
  return Call.get();
}

  static bool VSError(::clang::Sema* Sema, ::clang::Expr* E, ::llvm::StringRef Err) {
    DiagnosticsEngine& Diags = Sema->getDiagnostics();
    Diags.Report(E->getBeginLoc(),
                 Diags.getCustomDiagID(
                     ::clang::DiagnosticsEngine::Level::Error,
                     "ValueExtractionSynthesizer could not find: '%0'."))
        << Err;
    return false;
  }

  bool ValueExtractionSynthesizer::FindAndCacheRuntimeDecls(clang::Expr* E) {
    assert(!m_gClingVD && "Called multiple times!?");
    DeclContext* NSD = m_Context->getTranslationUnitDecl();
    clang::VarDecl* clingVD = nullptr;
    if (m_Sema->getLangOpts().CPlusPlus) {
      if (!(NSD = utils::Lookup::Namespace(m_Sema, "cling")))
        return VSError(m_Sema, E, "cling namespace");
      if (!(NSD = utils::Lookup::Namespace(m_Sema, "runtime", NSD)))
        return VSError(m_Sema, E, "cling::runtime namespace");
      if (!(clingVD = dyn_cast_or_null<VarDecl>(
                utils::Lookup::Named(m_Sema, "gCling", NSD))))
        return VSError(m_Sema, E, "cling::runtime::gCling");
      if (!(NSD = utils::Lookup::Namespace(m_Sema, "internal", NSD)))
        return VSError(m_Sema, E, "cling::runtime::internal namespace");
    }
    LookupResult R(*m_Sema, &m_Context->Idents.get("setValueNoAlloc"),
                   SourceLocation(), Sema::LookupOrdinaryName,
                   RedeclarationKind::ForVisibleRedeclaration);

    m_Sema->LookupQualifiedName(R, NSD);
    if (R.empty())
      return VSError(m_Sema, E, "cling::runtime::internal::setValueNoAlloc");

    const bool ADL = false;
    CXXScopeSpec CSS;
    m_UnresolvedNoAlloc = m_Sema->BuildDeclarationNameExpr(CSS, R, ADL).get();
    if (!m_UnresolvedNoAlloc)
      return VSError(m_Sema, E, "cling::runtime::internal::setValueNoAlloc");

    R.clear();
    R.setLookupName(&m_Context->Idents.get("setValueWithAlloc"));
    m_Sema->LookupQualifiedName(R, NSD);
    if (R.empty())
      return VSError(m_Sema, E, "cling::runtime::internal::setValueWithAlloc");

    m_UnresolvedWithAlloc = m_Sema->BuildDeclarationNameExpr(CSS, R, ADL).get();
    if (!m_UnresolvedWithAlloc)
      return VSError(m_Sema, E, "cling::runtime::internal::setValueWithAlloc");

    R.clear();
    R.setLookupName(&m_Context->Idents.get("copyArray"));
    m_Sema->LookupQualifiedName(R, NSD);
    // FIXME: In the case of the multiple interpreters (parent-child),
    // the child interpreter doesn't include the runtime universe.
    // The child interpreter will try to import this function from its
    // parent interpreter, but it will fail, because this is a template function.
    // Once the import of template functions becomes supported by clang,
    // this check can be de-activated.
    if (!m_isChildInterpreter && R.empty())
      return VSError(m_Sema, E, "cling::runtime::internal::copyArray");

    m_UnresolvedCopyArray = m_Sema->BuildDeclarationNameExpr(CSS, R, ADL).get();
    if (!m_UnresolvedCopyArray)
      return VSError(m_Sema, E, "cling::runtime::internal::copyArray");

    m_gClingVD = clingVD;
    return true;
  }
} // end namespace cling


// Provide implementation of the functions that ValueExtractionSynthesizer calls
namespace {

  static void dumpIfNoStorage(void* vpV) {
    const cling::Value& V = *(cling::Value*)vpV;
    // If the value copies over the temporary we must delay the printing until
    // the temporary gets copied over. For the rest of the temporaries we *must*
    // dump here because their lifetime will be gone otherwise. Eg.
    //
    // std::string f(); f().c_str() // have to dump during the same stmt.
    //
    assert(!V.needsManagedAllocation() && "Must contain non managed temporary");
    // assert(vpOn != (char)cling::CompilationOptions::VPAuto
    //        && "VPAuto must have been expanded earlier.");
    // if (vpOn == (char)cling::CompilationOptions::VPEnabled)
      V.dump();
  }

  ///\brief Allocate the Value and return the Value
  /// for an expression evaluated at the prompt.
  ///
  ///\param [in] interp - The cling::Interpreter to allocate the SToredValueRef.
  ///\param [in] vpQT - The opaque ptr for the ::clang::QualType of value stored.
  ///\param [out] vpStoredValRef - The Value that is allocated.
  static cling::Value&
  allocateStoredRefValueAndGetGV(void* vpI, void* vpSVR, void* vpQT) {
    cling::Interpreter* i = (cling::Interpreter*)vpI;
    ::clang::QualType QT = ::clang::QualType::getFromOpaquePtr(vpQT);
    cling::Value& SVR = *(cling::Value*)vpSVR;
    // if (vpSVR) SVR.dump();
    // Here the copy keeps the refcounted value alive.
    SVR = cling::Value(QT, *i);
    return SVR;
  }
}
namespace cling {
namespace runtime {
  namespace internal {
    CLING_LIB_EXPORT
    void setValueNoAlloc(void* vpI, void* vpSVR, void* vpQT) {
      // In cases of void we 'just' need to change the type of the value.
      allocateStoredRefValueAndGetGV(vpI, vpSVR, vpQT);
    }

    CLING_LIB_EXPORT
    void setValueNoAlloc(void* vpI, void* vpSVR, void* vpQT,
                         float value) {
      allocateStoredRefValueAndGetGV(vpI, vpSVR, vpQT).setFloat(value);
      dumpIfNoStorage(vpSVR);
    }

    CLING_LIB_EXPORT
    void setValueNoAlloc(void* vpI, void* vpSVR, void* vpQT,
                         double value) {
      allocateStoredRefValueAndGetGV(vpI, vpSVR, vpQT).setDouble(value);
      dumpIfNoStorage(vpSVR);
    }

    CLING_LIB_EXPORT
    void setValueNoAlloc(void* vpI, void* vpSVR, void* vpQT,
                         long double value) {
      allocateStoredRefValueAndGetGV(vpI, vpSVR, vpQT).setLongDouble(value);
      dumpIfNoStorage(vpSVR);
    }

    CLING_LIB_EXPORT
    void setValueNoAlloc(void* vpI, void* vpSVR, void* vpQT,
                         unsigned long long value) {
      allocateStoredRefValueAndGetGV(vpI, vpSVR, vpQT).setULongLong(value);
      dumpIfNoStorage(vpSVR);
    }

    CLING_LIB_EXPORT
    void setValueNoAlloc(void* vpI, void* vpSVR, void* vpQT,
                         const void* value){
      allocateStoredRefValueAndGetGV(vpI, vpSVR, vpQT)
        .setPtr(const_cast<void*>(value));
      dumpIfNoStorage(vpSVR);
    }

    CLING_LIB_EXPORT
    void* setValueWithAlloc(void* vpI, void* vpSVR, void* vpQT) {
      return allocateStoredRefValueAndGetGV(vpI, vpSVR, vpQT).getPtr();
    }
  } // end namespace internal
} // end namespace runtime
} // end namespace cling
