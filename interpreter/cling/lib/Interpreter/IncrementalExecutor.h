//--------------------------------------------------------------------*- C++ -*-
// CLING - the C++ LLVM-based InterpreterG :)
// author:  Axel Naumann <axel@cern.ch>
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#ifndef CLING_INCREMENTAL_EXECUTOR_H
#define CLING_INCREMENTAL_EXECUTOR_H

#include "BackendPasses.h"
#include "EnterUserCodeRAII.h"

#include "cling/Interpreter/DynamicLibraryManager.h"
#include "cling/Interpreter/InterpreterCallbacks.h"
#include "cling/Interpreter/Transaction.h"
#include "cling/Interpreter/Value.h"
#include "cling/Utils/Casting.h"
#include "cling/Utils/OrderedMap.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringRef.h"

#include <atomic>
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Module.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutorProcessControl.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/Error.h"
#include "llvm/Target/TargetMachine.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace clang {
  class DiagnosticsEngine;
  class CodeGenOptions;
  class CompilerInstance;
}

namespace llvm {
  class GlobalValue;
  class Module;
  class TargetMachine;
  namespace orc {
    class DefinitionGenerator;
  }
}

namespace cling {
  class DynamicLibraryManager;
  class Value;
  class Transaction;

  class SharedAtomicFlag {
  public:
    SharedAtomicFlag(bool UnlockedState)
        : Lock(std::make_shared<std::atomic<bool>>(UnlockedState)),
          LockedState(!UnlockedState) {}

    // FIXME: We don't lock recursively. Can we assert it?
    void lock() { Lock->store(LockedState); }
    void unlock() { Lock->store(!LockedState); }

    operator bool() const { return Lock->load(); }

  private:
    std::shared_ptr<std::atomic<bool>> Lock;
    const bool LockedState;
  };

  class IncrementalExecutor {
  private:
    // optimizer etc passes
    std::unique_ptr<BackendPasses> m_BackendPasses;

    ///\brief Whom to call upon invocation of user code.
    InterpreterCallbacks* m_Callbacks;

    ///\brief Helper that manages when the destructor of an object to be called.
    ///
    /// The object is registered first as an CXAAtExitElement and then cling
    /// takes the control of it's destruction.
    ///
    class CXAAtExitElement {
      ///\brief The function to be called.
      ///
      void (*m_Func)(void*);

      ///\brief The single argument passed to the function.
      ///
      void* m_Arg;

    public:
      ///\brief Constructs an element, whose destruction time will be managed by
      /// the interpreter. (By registering a function to be called by exit
      /// or when a shared library is unloaded.)
      ///
      /// Registers destructors for objects with static storage duration with
      /// the _cxa atexit function rather than the atexit function. This option
      /// is required for fully standards-compliant handling of static
      /// destructors(many of them created by cling), but will only work if
      /// your C library supports __cxa_atexit (means we have our own work
      /// around for Windows). More information about __cxa_atexit could be
      /// found in the Itanium C++ ABI spec.
      ///
      ///\param [in] func - The function to be called on exit or unloading of
      ///                   shared lib.(The destructor of the object.)
      ///\param [in] arg - The argument the func to be called with.
      ///\param [in] fromT - The unloading of this transaction will trigger the
      ///                    atexit function.
      ///
      CXAAtExitElement(void (*func)(void*), void* arg)
          : m_Func(func), m_Arg(arg) {}

      void operator()() const { (*m_Func)(m_Arg); }
    };

    ///\brief Atomic used as a spin lock to protect the access to m_AtExitFuncs
    ///
    /// AddAtExitFunc is used at the end of the 'interpreted' user code
    /// and before the calling framework has any change of taking back/again
    /// its lock protecting the access to cling, so we need to explicit protect
    /// again multiple conccurent access.
    std::atomic_flag m_AtExitFuncsSpinLock; // MSVC doesn't support = ATOMIC_FLAG_INIT;

    ///\brief Function registered via __cxa_atexit, atexit, or one of
    /// it's C++ overloads that should be run when a transaction is unloaded.
    ///
    using AtExitFunctions =
      utils::OrderedMap<const Transaction*, std::vector<CXAAtExitElement>>;
    AtExitFunctions m_AtExitFuncs;

    ///\brief Set of the symbols that the JIT couldn't resolve.
    ///
    mutable std::unordered_set<std::string> m_unresolvedSymbols;

#if 0 // See FIXME in IncrementalExecutor.cpp
    ///\brief The diagnostics engine, printing out issues coming from the
    /// incremental executor.
    clang::DiagnosticsEngine& m_Diags;
#endif

    /// Dynamic library manager object.
    ///
    DynamicLibraryManager m_DyLibManager;

  std::unique_ptr<llvm::orc::LLJIT> Jit;
  llvm::orc::SymbolMap m_InjectedSymbols;
  SharedAtomicFlag SkipHostProcessLookup;
  llvm::StringSet<> m_ForbidDlSymbols;
  llvm::orc::ResourceTrackerSP m_CurrentRT;

  /// FIXME: If the relation between modules and transactions is a bijection, the
  /// mapping via module pointers here is unnecessary. The transaction should
  /// store the resource tracker directly and pass it to `remove()` for
  /// unloading.
  std::map<const Transaction*, llvm::orc::ResourceTrackerSP> m_ResourceTrackers;
  std::map<const llvm::Module *, llvm::orc::ThreadSafeModule> m_CompiledModules;

  bool m_JITLink;
  // FIXME: Move TargetMachine ownership to BackendPasses
  std::unique_ptr<llvm::TargetMachine> m_TM;

  // TODO: We only need the context for materialization. Instead of defining it
  // here we might want to pass one in on a per-module basis.
  //
  // FIXME: Using a single context for all modules prevents concurrent
  // compilation.
  //
  llvm::orc::ThreadSafeContext SingleThreadedContext;

  public:
    enum ExecutionResult {
      kExeSuccess,
      kExeFunctionNotCompiled,
      kExeUnresolvedSymbols,
      kNumExeResults
    };

    IncrementalExecutor(clang::DiagnosticsEngine& diags,
                        const clang::CompilerInstance& CI,
                        void *ExtraLibHandle = nullptr,
                        bool Verbose = false);

    ~IncrementalExecutor();

    /// Register a different `IncrementalExecutor` object that can provide
    /// addresses for external symbols.  This is used by child interpreters to
    /// lookup symbols defined in the parent.
    void registerExternalIncrementalExecutor(IncrementalExecutor& IE);

    void setCallbacks(InterpreterCallbacks* callbacks);

    const DynamicLibraryManager& getDynamicLibraryManager() const {
      return const_cast<IncrementalExecutor*>(this)->m_DyLibManager;
    }
    DynamicLibraryManager& getDynamicLibraryManager() {
      return m_DyLibManager;
    }

    ///\brief Unload a set of JIT symbols.
    llvm::Error unloadModule(const Transaction& T) {
      return removeModule(T);
    }

    ///\brief Run the static initializers of all modules collected to far.
    ExecutionResult runStaticInitializersOnce(Transaction& T);

    ///\brief Runs all destructors bound to the given transaction and removes
    /// them from the list.
    ///\param[in] T - Transaction to which the dtors were bound.
    ///
    void runAndRemoveStaticDestructors(Transaction* T);

    ///\brief Runs a wrapper function.
    ExecutionResult executeWrapper(llvm::StringRef function,
                                   Value* returnValue = nullptr) const;
    ///\brief Replaces a symbol (function) to the execution engine.
    ///
    /// Allows runtime declaration of a function passing its pointer for being
    /// used by JIT generated code.
    ///
    /// @param[in] Name - The name of the symbol as known by the IR.
    /// @param[in] Address - The function pointer to register
    void replaceSymbol(const char* Name, void* Address);

    ///\brief Tells the execution to run all registered atexit functions once.
    ///
    /// This rountine should be used with caution only when an external process
    /// wants to carefully control the teardown. For example, if the process
    /// has registered its own atexit functions which need the interpreter
    /// service to be available when they are being executed.
    ///
    void runAtExitFuncs();

    ///\brief A more meaningful synonym of runAtExitFuncs when used in a more
    /// standard teardown.
    ///
    void shuttingDown() { runAtExitFuncs(); }

    ///\brief Gets the address of an existing global and whether it was JITted.
    ///
    /// JIT symbols might not be immediately convertible to e.g. a function
    /// pointer as their call setup is different.
    ///
    ///\param[in]  mangledName - the global's name
    ///\param[out] fromJIT - whether the symbol was JITted.
    ///
    void*
    getAddressOfGlobal(llvm::StringRef mangledName, bool *fromJIT = nullptr) const;

    ///\brief Return the address of a global from the JIT (as
    /// opposed to dynamic libraries). Forces the emission of the symbol if
    /// it has not happened yet.
    ///
    ///param[in] name - the mangled name of the global.
    void* getPointerToGlobalFromJIT(llvm::StringRef name) const;

    ///\brief Keep track of the entities whose dtor we need to call.
    ///
    void AddAtExitFunc(void (*func)(void*), void* arg, const Transaction* T);

  private:
    ///\brief Emit a llvm::Module to the JIT.
    ///
    /// @param[in] module - The module to pass to the execution engine.
    /// @param[in] optLevel - The optimization level to be used.
    void emitModule(Transaction &T) {
      if (m_BackendPasses)
        m_BackendPasses->runOnModule(*T.getModule(),
                                     T.getCompilationOpts().OptLevel);

      addModule(T);
    }

    ///\brief Report and empty m_unresolvedSymbols.
    ///\return true if m_unresolvedSymbols was non-empty.
    bool diagnoseUnresolvedSymbols(llvm::StringRef trigger,
                               llvm::StringRef title = llvm::StringRef()) const;

  public:
    /// Register a DefinitionGenerator to dynamically provide symbols for
    /// generated code that are not already available within the process.
    void addGenerator(std::unique_ptr<llvm::orc::DefinitionGenerator> G) {
      Jit->getMainJITDylib().addGenerator(std::move(G));
    }

    ///\brief Remember that the symbol could not be resolved by the JIT.
    void* HandleMissingFunction(const std::string& symbol) const;

    /// Return a `DefinitionGenerator` that can provide addresses for symbols
    /// reachable from this IncrementalExecutor object.  This function can be used in
    /// conjunction with `addGenerator()` to provide symbol resolution across
    /// diferent IncrementalExecutor instances.
    std::unique_ptr<llvm::orc::DefinitionGenerator> getGenerator();

      // FIXME: Accept a LLVMContext as well, e.g. the one that was used for the
    // particular module in Interpreter, CIFactory or BackendPasses (would be
    // more efficient)
    void addModule(Transaction& T);
  llvm::Error removeModule(const Transaction& T);

  /// Get the address of a symbol based on its IR name (as coming from clang's
  /// mangler). The IncludeHostSymbols parameter controls whether the lookup
  /// should include symbols from the host process (via dlsym) or not.
  void* getSymbolAddress(llvm::StringRef Name, bool IncludeHostSymbols) const;

  /// @brief Check whether the JIT already has emitted or knows how to emit
  /// a symbol based on its IR name (as coming from clang's mangler).
  bool doesSymbolAlreadyExist(llvm::StringRef UnmangledName);

  /// Inject a symbol with a known address. Name is not linker mangled, i.e.
  /// as known by the IR.
  llvm::JITTargetAddress addOrReplaceDefinition(llvm::StringRef Name,
                                                llvm::JITTargetAddress KnownAddr);

  llvm::Error runCtors() const {
    return Jit->initialize(Jit->getMainJITDylib());
  }

  /// @brief Get the TargetMachine used by the JIT.
  /// Non-const because BackendPasses need to update OptLevel.
  llvm::TargetMachine &getTargetMachine() { return *m_TM; }
  private:
    ///\brief Runs an initializer function.
    ExecutionResult executeInit(llvm::StringRef function) const {
      typedef void (*InitFun_t)();
      InitFun_t fun;
      ExecutionResult res = jitInitOrWrapper(function, fun);
      if (res != kExeSuccess)
        return res;
      EnterUserCodeRAII euc(m_Callbacks);
      (*fun)();
      return kExeSuccess;
    }

    template <class T>
    ExecutionResult jitInitOrWrapper(llvm::StringRef funcname, T& fun) const {
      void* fun_ptr = getSymbolAddress(funcname, false /*dlsym*/);

      // check if there is any unresolved symbol in the list
      if (diagnoseUnresolvedSymbols(funcname, "function") || !fun_ptr)
        return IncrementalExecutor::kExeUnresolvedSymbols;

      fun = reinterpret_cast<T>(fun_ptr);
      return IncrementalExecutor::kExeSuccess;
    }
  };
} // end cling
#endif // CLING_INCREMENTAL_EXECUTOR_H
