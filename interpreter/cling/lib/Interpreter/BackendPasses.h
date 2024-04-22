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

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/StandardInstrumentations.h"

#include <array>
#include <memory>

namespace llvm {
  class Function;
  class LLVMContext;
  class Module;
  class TargetMachine;
  namespace orc {
    class LLJIT;
    class ThreadSafeModule;
  } // namespace orc
}

namespace clang {
  class CodeGenOptions;
  class LangOptions;
  class TargetOptions;
}

namespace cling {
  ///\brief Runs passes on IR. Remove once we can migrate from ModuleBuilder to
  /// what's in clang's CodeGen/BackendUtil.
  class BackendPasses {
    llvm::TargetMachine& m_TM;
    llvm::orc::LLJIT& m_JIT;
    const clang::CodeGenOptions &m_CGOpts;
    std::map<const llvm::Module*, llvm::orc::ThreadSafeModule>&
        m_CompiledModules;

    void CreatePasses(int OptLevel, llvm::ModulePassManager& MPM,
                      llvm::LoopAnalysisManager& LAM,
                      llvm::FunctionAnalysisManager& FAM,
                      llvm::CGSCCAnalysisManager& CGAM,
                      llvm::ModuleAnalysisManager& MAM,
                      llvm::PassInstrumentationCallbacks& PIC,
                      llvm::StandardInstrumentations& SI);

  public:
    BackendPasses(const clang::CodeGenOptions& CGOpts, llvm::orc::LLJIT& JIT,
                  std::map<const llvm::Module*, llvm::orc::ThreadSafeModule>&
                      CompiledModules,
                  llvm::TargetMachine& TM);
    ~BackendPasses();

    void runOnModule(llvm::Module& M, int OptLevel);
  };
} // namespace cling

#endif // CLING_BACKENDPASSES_H
