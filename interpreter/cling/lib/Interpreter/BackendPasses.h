//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// author:  Vassil Vassilev <vvasilev@cern.ch>
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#ifndef CLING_BACKENDPASSES_H
#define CLING_BACKENDPASSES_H

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"

#include <array>
#include <memory>

namespace llvm {
  class Function;
  class LLVMContext;
  class Module;
  class PassManagerBuilder;
  class TargetMachine;

  namespace legacy {
    class FunctionPassManager;
    class PassManager;
  }
}

namespace clang {
  class CodeGenOptions;
  class LangOptions;
  class TargetOptions;
}

namespace cling {
  class IncrementalJIT;

  ///\brief Runs passes on IR. Remove once we can migrate from ModuleBuilder to
  /// what's in clang's CodeGen/BackendUtil.
  class BackendPasses {
    std::array<std::unique_ptr<llvm::legacy::PassManager>, 4> m_MPM;
    std::array<std::unique_ptr<llvm::legacy::FunctionPassManager>, 4> m_FPM;

    std::array<llvm::ModulePassManager, 4> m_nMPM;
    std::array<llvm::LoopAnalysisManager, 4> m_LAM;
    std::array<llvm::FunctionAnalysisManager, 4> m_FAM;
    std::array<llvm::CGSCCAnalysisManager, 4> m_CGAM;
    std::array<llvm::ModuleAnalysisManager, 4> m_MAM;

    llvm::TargetMachine& m_TM;
    IncrementalJIT &m_JIT;
    const clang::CodeGenOptions &m_CGOpts;

    void CreatePasses(llvm::Module& M, int OptLevel);

  public:
    BackendPasses(const clang::CodeGenOptions &CGOpts, IncrementalJIT &JIT,
                  llvm::TargetMachine& TM);
    ~BackendPasses();

    void runOnModule(llvm::Module& M, int OptLevel);
  };
}

#endif // CLING_BACKENDPASSES_H
