/**
 \file ROOT/RDF/ActionHelpers.hxx
 \ingroup dataframe
 \author Enrico Guiraud, CERN
 \author Danilo Piparo, CERN
 \date 2016-12
 \author Vincenzo Eduardo Padulano
 \date 2020-06
*/

/*************************************************************************
 * Copyright (C) 1995-2020, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#ifndef ROOT_RDFOPERATIONS
#define ROOT_RDFOPERATIONS

#include "Compression.h"
#include <string_view>
#include "ROOT/RVec.hxx"
#include "ROOT/TBufferMerger.hxx" // for SnapshotTTreeHelper
#include "ROOT/RDF/RCutFlowReport.hxx"
#include "ROOT/RDF/RSampleInfo.hxx"
#include "ROOT/RDF/Utils.hxx"
#include "ROOT/RSnapshotOptions.hxx"
#include "ROOT/TypeTraits.hxx"
#include "ROOT/RDF/RDisplay.hxx"
#include "RtypesCore.h"
#include "TBranch.h"
#include "TClassEdit.h"
#include "TClassRef.h"
#include "TDirectory.h"
#include "TError.h" // for R__ASSERT, Warning
#include "TFile.h"  // for SnapshotTTreeHelper
#include "TH1.h"
#include "TH3.h"
#include "TGraph.h"
#include "TGraphAsymmErrors.h"
#include "TLeaf.h"
#include "TObject.h"
#include "TTree.h"
#include "TTreeReader.h" // for SnapshotTTreeHelper
#include "TStatistic.h"
#include "ROOT/RDF/RActionImpl.hxx"
#include "ROOT/RDF/RMergeableValue.hxx"
#include "ROOT/RDF/RLoopManager.hxx"

#include "ROOT/RNTupleDS.hxx"
#include "ROOT/RNTupleWriter.hxx" // for SnapshotRNTupleHelper
#include "ROOT/RTTreeDS.hxx"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility> // std::index_sequence
#include <vector>
#include <iomanip>
#include <numeric> // std::accumulate in MeanHelper

/// \cond HIDDEN_SYMBOLS

namespace ROOT {
namespace Internal {
namespace RDF {
using namespace ROOT::TypeTraits;
using namespace ROOT::VecOps;
using namespace ROOT::RDF;
using namespace ROOT::Detail::RDF;

using Hist_t = ::TH1D;

class RBranchSet {
   std::vector<TBranch *> fBranches;
   std::vector<std::string> fNames;
   std::vector<bool> fIsCArray;

public:
   TBranch *Get(const std::string &name) const
   {
      auto it = std::find(fNames.begin(), fNames.end(), name);
      if (it == fNames.end())
         return nullptr;
      return fBranches[std::distance(fNames.begin(), it)];
   }

   bool IsCArray(const std::string &name) const
   {
      if (auto it = std::find(fNames.begin(), fNames.end(), name); it != fNames.end())
         return fIsCArray[std::distance(fNames.begin(), it)];
      return false;
   }

   void Insert(const std::string &name, TBranch *address, bool isCArray = false)
   {
      if (address == nullptr) {
         throw std::logic_error("Trying to insert a null branch address.");
      }
      if (std::find(fBranches.begin(), fBranches.end(), address) != fBranches.end()) {
         throw std::logic_error("Trying to insert a branch address that's already present.");
      }
      if (std::find(fNames.begin(), fNames.end(), name) != fNames.end()) {
         throw std::logic_error("Trying to insert a branch name that's already present.");
      }
      fNames.emplace_back(name);
      fBranches.emplace_back(address);
      fIsCArray.push_back(isCArray);
   }

   void Clear()
   {
      fBranches.clear();
      fNames.clear();
      fIsCArray.clear();
   }

   void AssertNoNullBranchAddresses()
   {
      std::vector<TBranch *> branchesWithNullAddress;
      std::copy_if(fBranches.begin(), fBranches.end(), std::back_inserter(branchesWithNullAddress),
                   [](TBranch *b) { return b->GetAddress() == nullptr; });

      if (branchesWithNullAddress.empty())
         return;

      // otherwise build error message and throw
      std::vector<std::string> missingBranchNames;
      std::transform(branchesWithNullAddress.begin(), branchesWithNullAddress.end(),
                     std::back_inserter(missingBranchNames), [](TBranch *b) { return b->GetName(); });
      std::string msg = "RDataFrame::Snapshot:";
      if (missingBranchNames.size() == 1) {
         msg += " branch " + missingBranchNames[0] +
                " is needed as it provides the size for one or more branches containing dynamically sized arrays, but "
                "it is";
      } else {
         msg += " branches ";
         for (const auto &bName : missingBranchNames)
            msg += bName + ", ";
         msg.resize(msg.size() - 2); // remove last ", "
         msg +=
            " are needed as they provide the size of other branches containing dynamically sized arrays, but they are";
      }
      msg += " not part of the set of branches that are being written out.";
      throw std::runtime_error(msg);
   }
};

/// The container type for each thread's partial result in an action helper
// We have to avoid to instantiate std::vector<bool> as that makes it impossible to return a reference to one of
// the thread-local results. In addition, a common definition for the type of the container makes it easy to swap
// the type of the underlying container if e.g. we see problems with false sharing of the thread-local results..
template <typename T>
using Results = std::conditional_t<std::is_same<T, bool>::value, std::deque<T>, std::vector<T>>;

template <typename F>
class R__CLING_PTRCHECK(off) ForeachSlotHelper : public RActionImpl<ForeachSlotHelper<F>> {
   F fCallable;

public:
   using ColumnTypes_t = RemoveFirstParameter_t<typename CallableTraits<F>::arg_types>;
   ForeachSlotHelper(F &&f) : fCallable(f) {}
   ForeachSlotHelper(ForeachSlotHelper &&) = default;
   ForeachSlotHelper(const ForeachSlotHelper &) = delete;

   void InitTask(TTreeReader *, unsigned int) {}

   template <typename... Args>
   void Exec(unsigned int slot, Args &&... args)
   {
      // check that the decayed types of Args are the same as the branch types
      static_assert(std::is_same<TypeList<std::decay_t<Args>...>, ColumnTypes_t>::value, "");
      fCallable(slot, std::forward<Args>(args)...);
   }

   void Initialize() { /* noop */}

   void Finalize() { /* noop */}

   std::string GetActionName() { return "ForeachSlot"; }
};

class R__CLING_PTRCHECK(off) CountHelper : public RActionImpl<CountHelper> {
   std::shared_ptr<ULong64_t> fResultCount;
   Results<ULong64_t> fCounts;

public:
   using ColumnTypes_t = TypeList<>;
   CountHelper(const std::shared_ptr<ULong64_t> &resultCount, const unsigned int nSlots);
   CountHelper(CountHelper &&) = default;
   CountHelper(const CountHelper &) = delete;
   void InitTask(TTreeReader *, unsigned int) {}
   void Exec(unsigned int slot);
   void Initialize() { /* noop */}
   void Finalize();

   // Helper functions for RMergeableValue
   std::unique_ptr<RMergeableValueBase> GetMergeableValue() const final
   {
      return std::make_unique<RMergeableCount>(*fResultCount);
   }

   ULong64_t &PartialUpdate(unsigned int slot);

   std::string GetActionName() { return "Count"; }

   CountHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<ULong64_t> *>(newResult);
      return CountHelper(result, fCounts.size());
   }
};

template <typename RNode_t>
class R__CLING_PTRCHECK(off) ReportHelper : public RActionImpl<ReportHelper<RNode_t>> {
   std::shared_ptr<RCutFlowReport> fReport;
   /// Non-owning pointer, never null. As usual, the node is owned by its children nodes (and therefore indirectly by
   /// the RAction corresponding to this action helper).
   RNode_t *fNode;
   bool fReturnEmptyReport;

public:
   using ColumnTypes_t = TypeList<>;
   ReportHelper(const std::shared_ptr<RCutFlowReport> &report, RNode_t *node, bool emptyRep)
      : fReport(report), fNode(node), fReturnEmptyReport(emptyRep){};
   ReportHelper(ReportHelper &&) = default;
   ReportHelper(const ReportHelper &) = delete;
   void InitTask(TTreeReader *, unsigned int) {}
   void Exec(unsigned int /* slot */) {}
   void Initialize() { /* noop */}
   void Finalize()
   {
      if (!fReturnEmptyReport)
         fNode->Report(*fReport);
   }

   std::string GetActionName() { return "Report"; }

   ReportHelper MakeNew(void *newResult, std::string_view variation = "nominal")
   {
      auto &&result = *static_cast<std::shared_ptr<RCutFlowReport> *>(newResult);
      return ReportHelper{result,
                          std::static_pointer_cast<RNode_t>(fNode->GetVariedFilter(std::string(variation))).get(),
                          fReturnEmptyReport};
   }
};

/// This helper fills TH1Ds for which no axes were specified by buffering the fill values to pick good axes limits.
///
/// TH1Ds have an automatic mechanism to pick good limits based on the first N entries they were filled with, but
/// that does not work in multi-thread event loops as it might yield histograms with incompatible binning in each
/// thread, making it impossible to merge the per-thread results.
/// Instead, this helper delays the decision on the axes limits until all threads have done processing, synchronizing
/// the decision on the limits as part of the merge operation.
class R__CLING_PTRCHECK(off) BufferedFillHelper : public RActionImpl<BufferedFillHelper> {
   // this sets a total initial size of 16 MB for the buffers (can increase)
   static constexpr unsigned int fgTotalBufSize = 2097152;
   using BufEl_t = double;
   using Buf_t = std::vector<BufEl_t>;

   std::vector<Buf_t> fBuffers;
   std::vector<Buf_t> fWBuffers;
   std::shared_ptr<Hist_t> fResultHist;
   unsigned int fNSlots;
   unsigned int fBufSize;
   /// Histograms containing "snapshots" of partial results. Non-null only if a registered callback requires it.
   Results<std::unique_ptr<Hist_t>> fPartialHists;
   Buf_t fMin;
   Buf_t fMax;

   void UpdateMinMax(unsigned int slot, double v);

public:
   BufferedFillHelper(const std::shared_ptr<Hist_t> &h, const unsigned int nSlots);
   BufferedFillHelper(BufferedFillHelper &&) = default;
   BufferedFillHelper(const BufferedFillHelper &) = delete;
   void InitTask(TTreeReader *, unsigned int) {}
   void Exec(unsigned int slot, double v);
   void Exec(unsigned int slot, double v, double w);

   template <typename T, std::enable_if_t<IsDataContainer<T>::value, int> = 0>
   void Exec(unsigned int slot, const T &vs)
   {
      auto &thisBuf = fBuffers[slot];
      // range-based for results in warnings on some compilers due to vector<bool>'s custom reference type
      for (auto v = vs.begin(); v != vs.end(); ++v) {
         UpdateMinMax(slot, *v);
         thisBuf.emplace_back(*v); // TODO: Can be optimised in case T == BufEl_t
      }
   }

   template <typename T, typename W, std::enable_if_t<IsDataContainer<T>::value && IsDataContainer<W>::value, int> = 0>
   void Exec(unsigned int slot, const T &vs, const W &ws)
   {
      auto &thisBuf = fBuffers[slot];

      for (auto &v : vs) {
         UpdateMinMax(slot, v);
         thisBuf.emplace_back(v);
      }

      auto &thisWBuf = fWBuffers[slot];
      for (auto &w : ws) {
         thisWBuf.emplace_back(w); // TODO: Can be optimised in case T == BufEl_t
      }
   }

   template <typename T, typename W, std::enable_if_t<IsDataContainer<T>::value && !IsDataContainer<W>::value, int> = 0>
   void Exec(unsigned int slot, const T &vs, const W w)
   {
      auto &thisBuf = fBuffers[slot];
      for (auto &v : vs) {
         UpdateMinMax(slot, v);
         thisBuf.emplace_back(v); // TODO: Can be optimised in case T == BufEl_t
      }

      auto &thisWBuf = fWBuffers[slot];
      thisWBuf.insert(thisWBuf.end(), vs.size(), w);
   }

   template <typename T, typename W, std::enable_if_t<IsDataContainer<W>::value && !IsDataContainer<T>::value, int> = 0>
   void Exec(unsigned int slot, const T v, const W &ws)
   {
      UpdateMinMax(slot, v);
      auto &thisBuf = fBuffers[slot];
      thisBuf.insert(thisBuf.end(), ws.size(), v);

      auto &thisWBuf = fWBuffers[slot];
      thisWBuf.insert(thisWBuf.end(), ws.begin(), ws.end());
   }

   Hist_t &PartialUpdate(unsigned int);

   void Initialize() { /* noop */}

   void Finalize();

   // Helper functions for RMergeableValue
   std::unique_ptr<RMergeableValueBase> GetMergeableValue() const final
   {
      return std::make_unique<RMergeableFill<Hist_t>>(*fResultHist);
   }

   std::string GetActionName()
   {
      return std::string(fResultHist->IsA()->GetName()) + "\\n" + std::string(fResultHist->GetName());
   }

   BufferedFillHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<Hist_t> *>(newResult);
      result->Reset();
      result->SetDirectory(nullptr);
      return BufferedFillHelper(result, fNSlots);
   }
};

// class which wraps a pointer and implements a no-op increment operator
template <typename T>
class ScalarConstIterator {
   const T *obj_;

public:
   using iterator_category = std::forward_iterator_tag;
   using difference_type = std::ptrdiff_t;
   using value_type = T;
   using pointer = T *;
   using reference = T &;
   ScalarConstIterator(const T *obj) : obj_(obj) {}
   const T &operator*() const { return *obj_; }
   ScalarConstIterator<T> &operator++() { return *this; }
};

// return unchanged value for scalar
template <typename T>
auto MakeBegin(const T &val)
{
   if constexpr (IsDataContainer<T>::value) {
      return std::begin(val);
   } else {
      return ScalarConstIterator<T>(&val);
   }
}

// return container size for containers, and 1 for scalars
template <typename T>
std::size_t GetSize(const T &val)
{
   if constexpr (IsDataContainer<T>::value) {
      return std::size(val);
   } else {
      return 1;
   }
}

// Helpers for dealing with histograms and similar:
template <typename H, typename = decltype(std::declval<H>().Reset())>
void ResetIfPossible(H *h)
{
   h->Reset();
}

void ResetIfPossible(TStatistic *h);
void ResetIfPossible(...);

void UnsetDirectoryIfPossible(TH1 *h);
void UnsetDirectoryIfPossible(...);

/// The generic Fill helper: it calls Fill on per-thread objects and then Merge to produce a final result.
/// For one-dimensional histograms, if no axes are specified, RDataFrame uses BufferedFillHelper instead.
template <typename HIST = Hist_t>
class R__CLING_PTRCHECK(off) FillHelper : public RActionImpl<FillHelper<HIST>> {
   std::vector<HIST *> fObjects;

   // Merge overload for types with Merge(TCollection*), like TH1s
   template <typename H, typename = std::enable_if_t<std::is_base_of<TObject, H>::value, int>>
   auto Merge(std::vector<H *> &objs, int /*toincreaseoverloadpriority*/)
      -> decltype(objs[0]->Merge((TCollection *)nullptr), void())
   {
      TList l;
      for (auto it = ++objs.begin(); it != objs.end(); ++it)
         l.Add(*it);
      objs[0]->Merge(&l);
   }

   // Merge overload for types with Merge(const std::vector&)
   template <typename H>
   auto Merge(std::vector<H *> &objs, double /*toloweroverloadpriority*/)
      -> decltype(objs[0]->Merge(std::vector<HIST *>{}), void())
   {
      objs[0]->Merge({++objs.begin(), objs.end()});
   }

   // Merge overload to error out in case no valid HIST::Merge method was detected
   template <typename T>
   void Merge(T, ...)
   {
      static_assert(sizeof(T) < 0,
                    "The type passed to Fill does not provide a Merge(TCollection*) or Merge(const std::vector&) method.");
   }

   template <std::size_t ColIdx, typename End_t, typename... Its>
   void ExecLoop(unsigned int slot, End_t end, Its... its)
   {
      for (auto *thisSlotH = fObjects[slot]; GetNthElement<ColIdx>(its...) != end; (std::advance(its, 1), ...)) {
         thisSlotH->Fill(*its...);
      }
   }

public:
   FillHelper(FillHelper &&) = default;
   FillHelper(const FillHelper &) = delete;

   FillHelper(const std::shared_ptr<HIST> &h, const unsigned int nSlots) : fObjects(nSlots, nullptr)
   {
      fObjects[0] = h.get();
      // Initialize all other slots
      for (unsigned int i = 1; i < nSlots; ++i) {
         fObjects[i] = new HIST(*fObjects[0]);
         UnsetDirectoryIfPossible(fObjects[i]);
      }
   }

   void InitTask(TTreeReader *, unsigned int) {}

   // no container arguments
   template <typename... ValTypes, std::enable_if_t<!Disjunction<IsDataContainer<ValTypes>...>::value, int> = 0>
   auto Exec(unsigned int slot, const ValTypes &...x) -> decltype(fObjects[slot]->Fill(x...), void())
   {
      fObjects[slot]->Fill(x...);
   }

   // at least one container argument
   template <typename... Xs, std::enable_if_t<Disjunction<IsDataContainer<Xs>...>::value, int> = 0>
   auto Exec(unsigned int slot, const Xs &...xs) -> decltype(fObjects[slot]->Fill(*MakeBegin(xs)...), void())
   {
      // array of bools keeping track of which inputs are containers
      constexpr std::array<bool, sizeof...(Xs)> isContainer{IsDataContainer<Xs>::value...};

      // index of the first container input
      constexpr std::size_t colidx = FindIdxTrue(isContainer);
      // if this happens, there is a bug in the implementation
      static_assert(colidx < sizeof...(Xs), "Error: index of collection-type argument not found.");

      // get the end iterator to the first container
      auto const xrefend = std::end(GetNthElement<colidx>(xs...));

      // array of container sizes (1 for scalars)
      std::array<std::size_t, sizeof...(xs)> sizes = {{GetSize(xs)...}};

      for (std::size_t i = 0; i < sizeof...(xs); ++i) {
         if (isContainer[i] && sizes[i] != sizes[colidx]) {
            throw std::runtime_error("Cannot fill histogram with values in containers of different sizes.");
         }
      }

      ExecLoop<colidx>(slot, xrefend, MakeBegin(xs)...);
   }

   template <typename T = HIST>
   void Exec(...)
   {
      static_assert(sizeof(T) < 0,
                    "When filling an object with RDataFrame (e.g. via a Fill action) the number or types of the "
                    "columns passed did not match the signature of the object's `Fill` method.");
   }

   void Initialize() { /* noop */}

   void Finalize()
   {
      if (fObjects.size() == 1)
         return;

      Merge(fObjects, /*toselectcorrectoverload=*/0);

      // delete the copies we created for the slots other than the first
      for (auto it = ++fObjects.begin(); it != fObjects.end(); ++it)
         delete *it;
   }

   HIST &PartialUpdate(unsigned int slot) { return *fObjects[slot]; }

   // Helper functions for RMergeableValue
   std::unique_ptr<RMergeableValueBase> GetMergeableValue() const final
   {
      return std::make_unique<RMergeableFill<HIST>>(*fObjects[0]);
   }

   // if the fObjects vector type is derived from TObject, return the name of the object
   template <typename T = HIST, std::enable_if_t<std::is_base_of<TObject, T>::value, int> = 0>
   std::string GetActionName()
   {
      return std::string(fObjects[0]->IsA()->GetName()) + "\\n" + std::string(fObjects[0]->GetName());
   }

   // if fObjects is not derived from TObject, indicate it is some other object
   template <typename T = HIST, std::enable_if_t<!std::is_base_of<TObject, T>::value, int> = 0>
   std::string GetActionName()
   {
      return "Fill custom object";
   }

   template <typename H = HIST>
   FillHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<H> *>(newResult);
      ResetIfPossible(result.get());
      UnsetDirectoryIfPossible(result.get());
      return FillHelper(result, fObjects.size());
   }
};

class R__CLING_PTRCHECK(off) FillTGraphHelper : public ROOT::Detail::RDF::RActionImpl<FillTGraphHelper> {
public:
   using Result_t = ::TGraph;

private:
   std::vector<::TGraph *> fGraphs;

public:
   FillTGraphHelper(FillTGraphHelper &&) = default;
   FillTGraphHelper(const FillTGraphHelper &) = delete;

   FillTGraphHelper(const std::shared_ptr<::TGraph> &g, const unsigned int nSlots) : fGraphs(nSlots, nullptr)
   {
      fGraphs[0] = g.get();
      // Initialize all other slots
      for (unsigned int i = 1; i < nSlots; ++i) {
         fGraphs[i] = new TGraph(*fGraphs[0]);
      }
   }

   void Initialize() {}
   void InitTask(TTreeReader *, unsigned int) {}

   // case: both types are container types
   template <typename X0, typename X1,
             std::enable_if_t<IsDataContainer<X0>::value && IsDataContainer<X1>::value, int> = 0>
   void Exec(unsigned int slot, const X0 &x0s, const X1 &x1s)
   {
      if (x0s.size() != x1s.size()) {
         throw std::runtime_error("Cannot fill Graph with values in containers of different sizes.");
      }
      auto *thisSlotG = fGraphs[slot];
      auto x0sIt = std::begin(x0s);
      const auto x0sEnd = std::end(x0s);
      auto x1sIt = std::begin(x1s);
      for (; x0sIt != x0sEnd; x0sIt++, x1sIt++) {
         thisSlotG->SetPoint(thisSlotG->GetN(), *x0sIt, *x1sIt);
      }
   }

   // case: both types are non-container types, e.g. scalars
   template <typename X0, typename X1,
             std::enable_if_t<!IsDataContainer<X0>::value && !IsDataContainer<X1>::value, int> = 0>
   void Exec(unsigned int slot, X0 x0, X1 x1)
   {
      auto thisSlotG = fGraphs[slot];
      thisSlotG->SetPoint(thisSlotG->GetN(), x0, x1);
   }

   // case: types are combination of containers and non-containers
   // this is not supported, error out
   template <typename X0, typename X1, typename... ExtraArgsToLowerPriority>
   void Exec(unsigned int, X0, X1, ExtraArgsToLowerPriority...)
   {
      throw std::runtime_error("Graph was applied to a mix of scalar values and collections. This is not supported.");
   }

   void Finalize()
   {
      const auto nSlots = fGraphs.size();
      auto resGraph = fGraphs[0];
      TList l;
      l.SetOwner(); // The list will free the memory associated to its elements upon destruction
      for (unsigned int slot = 1; slot < nSlots; ++slot) {
         l.Add(fGraphs[slot]);
      }
      resGraph->Merge(&l);
   }

   // Helper functions for RMergeableValue
   std::unique_ptr<RMergeableValueBase> GetMergeableValue() const final
   {
      return std::make_unique<RMergeableFill<Result_t>>(*fGraphs[0]);
   }

   std::string GetActionName() { return "Graph"; }

   Result_t &PartialUpdate(unsigned int slot) { return *fGraphs[slot]; }

   FillTGraphHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<TGraph> *>(newResult);
      result->Set(0);
      return FillTGraphHelper(result, fGraphs.size());
   }
};

class R__CLING_PTRCHECK(off) FillTGraphAsymmErrorsHelper
   : public ROOT::Detail::RDF::RActionImpl<FillTGraphAsymmErrorsHelper> {
public:
   using Result_t = ::TGraphAsymmErrors;

private:
   std::vector<::TGraphAsymmErrors *> fGraphAsymmErrors;

public:
   FillTGraphAsymmErrorsHelper(FillTGraphAsymmErrorsHelper &&) = default;
   FillTGraphAsymmErrorsHelper(const FillTGraphAsymmErrorsHelper &) = delete;

   FillTGraphAsymmErrorsHelper(const std::shared_ptr<::TGraphAsymmErrors> &g, const unsigned int nSlots)
      : fGraphAsymmErrors(nSlots, nullptr)
   {
      fGraphAsymmErrors[0] = g.get();
      // Initialize all other slots
      for (unsigned int i = 1; i < nSlots; ++i) {
         fGraphAsymmErrors[i] = new TGraphAsymmErrors(*fGraphAsymmErrors[0]);
      }
   }

   void Initialize() {}
   void InitTask(TTreeReader *, unsigned int) {}

   // case: all types are container types
   template <
      typename X, typename Y, typename EXL, typename EXH, typename EYL, typename EYH,
      std::enable_if_t<IsDataContainer<X>::value && IsDataContainer<Y>::value && IsDataContainer<EXL>::value &&
                          IsDataContainer<EXH>::value && IsDataContainer<EYL>::value && IsDataContainer<EYH>::value,
                       int> = 0>
   void
   Exec(unsigned int slot, const X &xs, const Y &ys, const EXL &exls, const EXH &exhs, const EYL &eyls, const EYH &eyhs)
   {
      if ((xs.size() != ys.size()) || (xs.size() != exls.size()) || (xs.size() != exhs.size()) ||
          (xs.size() != eyls.size()) || (xs.size() != eyhs.size())) {
         throw std::runtime_error("Cannot fill GraphAsymmErrors with values in containers of different sizes.");
      }
      auto *thisSlotG = fGraphAsymmErrors[slot];
      auto xsIt = std::begin(xs);
      auto ysIt = std::begin(ys);
      auto exlsIt = std::begin(exls);
      auto exhsIt = std::begin(exhs);
      auto eylsIt = std::begin(eyls);
      auto eyhsIt = std::begin(eyhs);
      while (xsIt != std::end(xs)) {
         const auto n = thisSlotG->GetN(); // must use the same `n` for SetPoint and SetPointError
         thisSlotG->SetPoint(n, *xsIt++, *ysIt++);
         thisSlotG->SetPointError(n, *exlsIt++, *exhsIt++, *eylsIt++, *eyhsIt++);
      }
   }

   // case: all types are non-container types, e.g. scalars
   template <
      typename X, typename Y, typename EXL, typename EXH, typename EYL, typename EYH,
      std::enable_if_t<!IsDataContainer<X>::value && !IsDataContainer<Y>::value && !IsDataContainer<EXL>::value &&
                          !IsDataContainer<EXH>::value && !IsDataContainer<EYL>::value && !IsDataContainer<EYH>::value,
                       int> = 0>
   void Exec(unsigned int slot, X x, Y y, EXL exl, EXH exh, EYL eyl, EYH eyh)
   {
      auto thisSlotG = fGraphAsymmErrors[slot];
      const auto n = thisSlotG->GetN();
      thisSlotG->SetPoint(n, x, y);
      thisSlotG->SetPointError(n, exl, exh, eyl, eyh);
   }

   // case: types are combination of containers and non-containers
   // this is not supported, error out
   template <typename X, typename Y, typename EXL, typename EXH, typename EYL, typename EYH,
             typename... ExtraArgsToLowerPriority>
   void Exec(unsigned int, X, Y, EXL, EXH, EYL, EYH, ExtraArgsToLowerPriority...)
   {
      throw std::runtime_error(
         "GraphAsymmErrors was applied to a mix of scalar values and collections. This is not supported.");
   }

   void Finalize()
   {
      const auto nSlots = fGraphAsymmErrors.size();
      auto resGraphAsymmErrors = fGraphAsymmErrors[0];
      TList l;
      l.SetOwner(); // The list will free the memory associated to its elements upon destruction
      for (unsigned int slot = 1; slot < nSlots; ++slot) {
         l.Add(fGraphAsymmErrors[slot]);
      }
      resGraphAsymmErrors->Merge(&l);
   }

   // Helper functions for RMergeableValue
   std::unique_ptr<RMergeableValueBase> GetMergeableValue() const final
   {
      return std::make_unique<RMergeableFill<Result_t>>(*fGraphAsymmErrors[0]);
   }

   std::string GetActionName() { return "GraphAsymmErrors"; }

   Result_t &PartialUpdate(unsigned int slot) { return *fGraphAsymmErrors[slot]; }

   FillTGraphAsymmErrorsHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<TGraphAsymmErrors> *>(newResult);
      result->Set(0);
      return FillTGraphAsymmErrorsHelper(result, fGraphAsymmErrors.size());
   }
};

/// A FillHelper for classes supporting the FillThreadSafe function.
template <typename HIST>
class R__CLING_PTRCHECK(off) ThreadSafeFillHelper : public RActionImpl<ThreadSafeFillHelper<HIST>> {
   std::vector<std::shared_ptr<HIST>> fObjects;
   std::vector<std::unique_ptr<std::mutex>> fMutexPtrs;

   // This overload matches if the function exists:
   template <typename T, typename... Args>
   auto TryCallFillThreadSafe(T &object, std::mutex &, int /*dummy*/, Args... args)
      -> decltype(ROOT::Internal::FillThreadSafe(object, args...), void())
   {
      ROOT::Internal::FillThreadSafe(object, args...);
   }
   // This one has lower precedence because of the dummy argument, and uses a lock
   template <typename T, typename... Args>
   auto TryCallFillThreadSafe(T &object, std::mutex &mutex, char /*dummy*/, Args... args)
   {
      std::scoped_lock lock{mutex};
      object.Fill(args...);
   }

   template <std::size_t ColIdx, typename End_t, typename... Its>
   void ExecLoop(unsigned int slot, End_t end, Its... its)
   {
      const auto localSlot = slot % fObjects.size();
      for (; GetNthElement<ColIdx>(its...) != end; (std::advance(its, 1), ...)) {
         TryCallFillThreadSafe(*fObjects[localSlot], *fMutexPtrs[localSlot], 0, *its...);
      }
   }

public:
   ThreadSafeFillHelper(ThreadSafeFillHelper &&) = default;
   ThreadSafeFillHelper(const ThreadSafeFillHelper &) = delete;

   ThreadSafeFillHelper(const std::shared_ptr<HIST> &h, const unsigned int nSlots)
   {
      fObjects.resize(nSlots);
      fObjects.front() = h;

      std::generate(fObjects.begin() + 1, fObjects.end(), [h]() {
         auto hist = std::make_shared<HIST>(*h);
         UnsetDirectoryIfPossible(hist.get());
         return hist;
      });
      fMutexPtrs.resize(nSlots);
      std::generate(fMutexPtrs.begin(), fMutexPtrs.end(), []() { return std::make_unique<std::mutex>(); });
   }

   void InitTask(TTreeReader *, unsigned int) {}

   // no container arguments
   template <typename... ValTypes, std::enable_if_t<!Disjunction<IsDataContainer<ValTypes>...>::value, int> = 0>
   void Exec(unsigned int slot, const ValTypes &...x)
   {
      const auto localSlot = slot % fObjects.size();
      TryCallFillThreadSafe(*fObjects[localSlot], *fMutexPtrs[localSlot], 0, x...);
   }

   // at least one container argument
   template <typename... Xs, std::enable_if_t<Disjunction<IsDataContainer<Xs>...>::value, int> = 0>
   void Exec(unsigned int slot, const Xs &...xs)
   {
      // array of bools keeping track of which inputs are containers
      constexpr std::array<bool, sizeof...(Xs)> isContainer{IsDataContainer<Xs>::value...};

      // index of the first container input
      constexpr std::size_t colidx = FindIdxTrue(isContainer);
      // if this happens, there is a bug in the implementation
      static_assert(colidx < sizeof...(Xs), "Error: index of collection-type argument not found.");

      // get the end iterator to the first container
      auto const xrefend = std::end(GetNthElement<colidx>(xs...));

      // array of container sizes (1 for scalars)
      std::array<std::size_t, sizeof...(xs)> sizes = {{GetSize(xs)...}};

      for (std::size_t i = 0; i < sizeof...(xs); ++i) {
         if (isContainer[i] && sizes[i] != sizes[colidx]) {
            throw std::runtime_error("Cannot fill histogram with values in containers of different sizes.");
         }
      }

      ExecLoop<colidx>(slot, xrefend, MakeBegin(xs)...);
   }

   template <typename T = HIST>
   void Exec(...)
   {
      static_assert(sizeof(T) < 0,
                    "When filling an object with RDataFrame (e.g. via a Fill action) the number or types of the "
                    "columns passed did not match the signature of the object's `FillThreadSafe` method.");
   }

   void Initialize() { /* noop */ }

   void Finalize()
   {
      if (fObjects.size() > 1) {
         TList list;
         for (auto it = fObjects.cbegin() + 1; it != fObjects.end(); ++it) {
            list.Add(it->get());
         }
         fObjects[0]->Merge(&list);
      }

      fObjects.resize(1);
      fMutexPtrs.clear();
   }

   // Helper function for RMergeableValue
   std::unique_ptr<RMergeableValueBase> GetMergeableValue() const final
   {
      return std::make_unique<RMergeableFill<HIST>>(*fObjects[0]);
   }

   // if the fObjects vector type is derived from TObject, return the name of the object
   template <typename T = HIST, std::enable_if_t<std::is_base_of<TObject, T>::value, int> = 0>
   std::string GetActionName()
   {
      return std::string(fObjects[0]->IsA()->GetName()) + "\\n" + std::string(fObjects[0]->GetName());
   }

   template <typename H = HIST>
   ThreadSafeFillHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<H> *>(newResult);
      ResetIfPossible(result.get());
      UnsetDirectoryIfPossible(result.get());
      return ThreadSafeFillHelper(result, fObjects.size());
   }
};

// In case of the take helper we have 4 cases:
// 1. The column is not an RVec, the collection is not a vector
// 2. The column is not an RVec, the collection is a vector
// 3. The column is an RVec, the collection is not a vector
// 4. The column is an RVec, the collection is a vector

template <typename V, typename COLL>
void FillColl(V&& v, COLL& c) {
   c.emplace_back(v);
}

// Use push_back for bool since some compilers do not support emplace_back.
template <typename COLL>
void FillColl(bool v, COLL& c) {
   c.push_back(v);
}

// Case 1.: The column is not an RVec, the collection is not a vector
// No optimisations, no transformations: just copies.
template <typename RealT_t, typename T, typename COLL>
class R__CLING_PTRCHECK(off) TakeHelper : public RActionImpl<TakeHelper<RealT_t, T, COLL>> {
   Results<std::shared_ptr<COLL>> fColls;

public:
   using ColumnTypes_t = TypeList<T>;
   TakeHelper(const std::shared_ptr<COLL> &resultColl, const unsigned int nSlots)
   {
      fColls.emplace_back(resultColl);
      for (unsigned int i = 1; i < nSlots; ++i)
         fColls.emplace_back(std::make_shared<COLL>());
   }
   TakeHelper(TakeHelper &&);
   TakeHelper(const TakeHelper &) = delete;

   void InitTask(TTreeReader *, unsigned int) {}

   void Exec(unsigned int slot, T &v) { FillColl(v, *fColls[slot]); }

   void Initialize() { /* noop */}

   void Finalize()
   {
      auto rColl = fColls[0];
      for (unsigned int i = 1; i < fColls.size(); ++i) {
         const auto &coll = fColls[i];
         const auto end = coll->end();
         // Use an explicit loop here to prevent compiler warnings introduced by
         // clang's range-based loop analysis and vector<bool> references.
         for (auto j = coll->begin(); j != end; j++) {
            FillColl(*j, *rColl);
         }
      }
   }

   COLL &PartialUpdate(unsigned int slot) { return *fColls[slot].get(); }

   std::string GetActionName() { return "Take"; }

   TakeHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<COLL> *>(newResult);
      result->clear();
      return TakeHelper(result, fColls.size());
   }
};

// Case 2.: The column is not an RVec, the collection is a vector
// Optimisations, no transformations: just copies.
template <typename RealT_t, typename T>
class R__CLING_PTRCHECK(off) TakeHelper<RealT_t, T, std::vector<T>>
   : public RActionImpl<TakeHelper<RealT_t, T, std::vector<T>>> {
   Results<std::shared_ptr<std::vector<T>>> fColls;

public:
   using ColumnTypes_t = TypeList<T>;
   TakeHelper(const std::shared_ptr<std::vector<T>> &resultColl, const unsigned int nSlots)
   {
      fColls.emplace_back(resultColl);
      for (unsigned int i = 1; i < nSlots; ++i) {
         auto v = std::make_shared<std::vector<T>>();
         v->reserve(1024);
         fColls.emplace_back(v);
      }
   }
   TakeHelper(TakeHelper &&);
   TakeHelper(const TakeHelper &) = delete;

   void InitTask(TTreeReader *, unsigned int) {}

   void Exec(unsigned int slot, T &v) { FillColl(v, *fColls[slot]); }

   void Initialize() { /* noop */}

   // This is optimised to treat vectors
   void Finalize()
   {
      ULong64_t totSize = 0;
      for (auto &coll : fColls)
         totSize += coll->size();
      auto rColl = fColls[0];
      rColl->reserve(totSize);
      for (unsigned int i = 1; i < fColls.size(); ++i) {
         auto &coll = fColls[i];
         rColl->insert(rColl->end(), coll->begin(), coll->end());
      }
   }

   std::vector<T> &PartialUpdate(unsigned int slot) { return *fColls[slot]; }

   std::string GetActionName() { return "Take"; }

   TakeHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<std::vector<T>> *>(newResult);
      result->clear();
      return TakeHelper(result, fColls.size());
   }
};

// Case 3.: The column is a RVec, the collection is not a vector
// No optimisations, transformations from RVecs to vectors
template <typename RealT_t, typename COLL>
class R__CLING_PTRCHECK(off) TakeHelper<RealT_t, RVec<RealT_t>, COLL>
   : public RActionImpl<TakeHelper<RealT_t, RVec<RealT_t>, COLL>> {
   Results<std::shared_ptr<COLL>> fColls;

public:
   using ColumnTypes_t = TypeList<RVec<RealT_t>>;
   TakeHelper(const std::shared_ptr<COLL> &resultColl, const unsigned int nSlots)
   {
      fColls.emplace_back(resultColl);
      for (unsigned int i = 1; i < nSlots; ++i)
         fColls.emplace_back(std::make_shared<COLL>());
   }
   TakeHelper(TakeHelper &&);
   TakeHelper(const TakeHelper &) = delete;

   void InitTask(TTreeReader *, unsigned int) {}

   void Exec(unsigned int slot, RVec<RealT_t> av) { fColls[slot]->emplace_back(av.begin(), av.end()); }

   void Initialize() { /* noop */}

   void Finalize()
   {
      auto rColl = fColls[0];
      for (unsigned int i = 1; i < fColls.size(); ++i) {
         auto &coll = fColls[i];
         for (auto &v : *coll) {
            rColl->emplace_back(v);
         }
      }
   }

   std::string GetActionName() { return "Take"; }

   TakeHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<COLL> *>(newResult);
      result->clear();
      return TakeHelper(result, fColls.size());
   }
};

// Case 4.: The column is an RVec, the collection is a vector
// Optimisations, transformations from RVecs to vectors
template <typename RealT_t>
class R__CLING_PTRCHECK(off) TakeHelper<RealT_t, RVec<RealT_t>, std::vector<RealT_t>>
   : public RActionImpl<TakeHelper<RealT_t, RVec<RealT_t>, std::vector<RealT_t>>> {

   Results<std::shared_ptr<std::vector<std::vector<RealT_t>>>> fColls;

public:
   using ColumnTypes_t = TypeList<RVec<RealT_t>>;
   TakeHelper(const std::shared_ptr<std::vector<std::vector<RealT_t>>> &resultColl, const unsigned int nSlots)
   {
      fColls.emplace_back(resultColl);
      for (unsigned int i = 1; i < nSlots; ++i) {
         auto v = std::make_shared<std::vector<RealT_t>>();
         v->reserve(1024);
         fColls.emplace_back(v);
      }
   }
   TakeHelper(TakeHelper &&);
   TakeHelper(const TakeHelper &) = delete;

   void InitTask(TTreeReader *, unsigned int) {}

   void Exec(unsigned int slot, RVec<RealT_t> av) { fColls[slot]->emplace_back(av.begin(), av.end()); }

   void Initialize() { /* noop */}

   // This is optimised to treat vectors
   void Finalize()
   {
      ULong64_t totSize = 0;
      for (auto &coll : fColls)
         totSize += coll->size();
      auto rColl = fColls[0];
      rColl->reserve(totSize);
      for (unsigned int i = 1; i < fColls.size(); ++i) {
         auto &coll = fColls[i];
         rColl->insert(rColl->end(), coll->begin(), coll->end());
      }
   }

   std::string GetActionName() { return "Take"; }

   TakeHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<typename decltype(fColls)::value_type *>(newResult);
      result->clear();
      return TakeHelper(result, fColls.size());
   }
};

// Extern templates for TakeHelper
// NOTE: The move-constructor of specializations declared as extern templates
// must be defined out of line, otherwise cling fails to find its symbol.
template <typename RealT_t, typename T, typename COLL>
TakeHelper<RealT_t, T, COLL>::TakeHelper(TakeHelper<RealT_t, T, COLL> &&) = default;
template <typename RealT_t, typename T>
TakeHelper<RealT_t, T, std::vector<T>>::TakeHelper(TakeHelper<RealT_t, T, std::vector<T>> &&) = default;
template <typename RealT_t, typename COLL>
TakeHelper<RealT_t, RVec<RealT_t>, COLL>::TakeHelper(TakeHelper<RealT_t, RVec<RealT_t>, COLL> &&) = default;
template <typename RealT_t>
TakeHelper<RealT_t, RVec<RealT_t>, std::vector<RealT_t>>::TakeHelper(TakeHelper<RealT_t, RVec<RealT_t>, std::vector<RealT_t>> &&) = default;

// External templates are disabled for gcc5 since this version wrongly omits the C++11 ABI attribute
#if __GNUC__ > 5
extern template class TakeHelper<bool, bool, std::vector<bool>>;
extern template class TakeHelper<unsigned int, unsigned int, std::vector<unsigned int>>;
extern template class TakeHelper<unsigned long, unsigned long, std::vector<unsigned long>>;
extern template class TakeHelper<unsigned long long, unsigned long long, std::vector<unsigned long long>>;
extern template class TakeHelper<int, int, std::vector<int>>;
extern template class TakeHelper<long, long, std::vector<long>>;
extern template class TakeHelper<long long, long long, std::vector<long long>>;
extern template class TakeHelper<float, float, std::vector<float>>;
extern template class TakeHelper<double, double, std::vector<double>>;
#endif

template <typename ResultType>
class R__CLING_PTRCHECK(off) MinHelper : public RActionImpl<MinHelper<ResultType>> {
   std::shared_ptr<ResultType> fResultMin;
   Results<ResultType> fMins;

public:
   MinHelper(MinHelper &&) = default;
   MinHelper(const std::shared_ptr<ResultType> &minVPtr, const unsigned int nSlots)
      : fResultMin(minVPtr), fMins(nSlots, std::numeric_limits<ResultType>::max())
   {
   }

   void Exec(unsigned int slot, ResultType v) { fMins[slot] = std::min(v, fMins[slot]); }

   void InitTask(TTreeReader *, unsigned int) {}

   template <typename T, std::enable_if_t<IsDataContainer<T>::value, int> = 0>
   void Exec(unsigned int slot, const T &vs)
   {
      for (auto &&v : vs)
         fMins[slot] = std::min(static_cast<ResultType>(v), fMins[slot]);
   }

   void Initialize() { /* noop */}

   void Finalize()
   {
      *fResultMin = std::numeric_limits<ResultType>::max();
      for (auto &m : fMins)
         *fResultMin = std::min(m, *fResultMin);
   }

   // Helper functions for RMergeableValue
   std::unique_ptr<RMergeableValueBase> GetMergeableValue() const final
   {
      return std::make_unique<RMergeableMin<ResultType>>(*fResultMin);
   }

   ResultType &PartialUpdate(unsigned int slot) { return fMins[slot]; }

   std::string GetActionName() { return "Min"; }

   MinHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<ResultType> *>(newResult);
      return MinHelper(result, fMins.size());
   }
};

template <typename ResultType>
class R__CLING_PTRCHECK(off) MaxHelper : public RActionImpl<MaxHelper<ResultType>> {
   std::shared_ptr<ResultType> fResultMax;
   Results<ResultType> fMaxs;

public:
   MaxHelper(MaxHelper &&) = default;
   MaxHelper(const MaxHelper &) = delete;
   MaxHelper(const std::shared_ptr<ResultType> &maxVPtr, const unsigned int nSlots)
      : fResultMax(maxVPtr), fMaxs(nSlots, std::numeric_limits<ResultType>::lowest())
   {
   }

   void InitTask(TTreeReader *, unsigned int) {}
   void Exec(unsigned int slot, ResultType v) { fMaxs[slot] = std::max(v, fMaxs[slot]); }

   template <typename T, std::enable_if_t<IsDataContainer<T>::value, int> = 0>
   void Exec(unsigned int slot, const T &vs)
   {
      for (auto &&v : vs)
         fMaxs[slot] = std::max(static_cast<ResultType>(v), fMaxs[slot]);
   }

   void Initialize() { /* noop */}

   void Finalize()
   {
      *fResultMax = std::numeric_limits<ResultType>::lowest();
      for (auto &m : fMaxs) {
         *fResultMax = std::max(m, *fResultMax);
      }
   }

   // Helper functions for RMergeableValue
   std::unique_ptr<RMergeableValueBase> GetMergeableValue() const final
   {
      return std::make_unique<RMergeableMax<ResultType>>(*fResultMax);
   }

   ResultType &PartialUpdate(unsigned int slot) { return fMaxs[slot]; }

   std::string GetActionName() { return "Max"; }

   MaxHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<ResultType> *>(newResult);
      return MaxHelper(result, fMaxs.size());
   }
};

template <typename ResultType>
class R__CLING_PTRCHECK(off) SumHelper : public RActionImpl<SumHelper<ResultType>> {
   std::shared_ptr<ResultType> fResultSum;
   Results<ResultType> fSums;
   Results<ResultType> fCompensations;

   /// Evaluate neutral element for this type and the sum operation.
   /// This is assumed to be any_value - any_value if operator- is defined
   /// for the type, otherwise a default-constructed ResultType{} is used.
   template <typename T = ResultType>
   auto NeutralElement(const T &v, int /*overloadresolver*/) -> decltype(v - v)
   {
      return v - v;
   }

   template <typename T = ResultType, typename Dummy = int>
   ResultType NeutralElement(const T &, Dummy) // this overload has lower priority thanks to the template arg
   {
      return ResultType{};
   }

public:
   SumHelper(SumHelper &&) = default;
   SumHelper(const SumHelper &) = delete;
   SumHelper(const std::shared_ptr<ResultType> &sumVPtr, const unsigned int nSlots)
      : fResultSum(sumVPtr), fSums(nSlots, NeutralElement(*sumVPtr, -1)),
        fCompensations(nSlots, NeutralElement(*sumVPtr, -1))
   {
   }
   void InitTask(TTreeReader *, unsigned int) {}

   void Exec(unsigned int slot, ResultType x)
   {
      // Kahan Sum:
      ResultType y = x - fCompensations[slot];
      ResultType t = fSums[slot] + y;
      fCompensations[slot] = (t - fSums[slot]) - y;
      fSums[slot] = t;
   }

   template <typename T, std::enable_if_t<IsDataContainer<T>::value, int> = 0>
   void Exec(unsigned int slot, const T &vs)
   {
      for (auto &&v : vs) {
         Exec(slot, v);
      }
   }

   void Initialize() { /* noop */}

   void Finalize()
   {
      ResultType sum(NeutralElement(ResultType{}, -1));
      ResultType compensation(NeutralElement(ResultType{}, -1));
      ResultType y(NeutralElement(ResultType{}, -1));
      ResultType t(NeutralElement(ResultType{}, -1));
      for (auto &m : fSums) {
         // Kahan Sum:
         y = m - compensation;
         t = sum + y;
         compensation = (t - sum) - y;
         sum = t;
      }
      *fResultSum += sum;
   }

   // Helper functions for RMergeableValue
   std::unique_ptr<RMergeableValueBase> GetMergeableValue() const final
   {
      return std::make_unique<RMergeableSum<ResultType>>(*fResultSum);
   }

   ResultType &PartialUpdate(unsigned int slot) { return fSums[slot]; }

   std::string GetActionName() { return "Sum"; }

   SumHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<ResultType> *>(newResult);
      *result = NeutralElement(*result, -1);
      return SumHelper(result, fSums.size());
   }
};

class R__CLING_PTRCHECK(off) MeanHelper : public RActionImpl<MeanHelper> {
   std::shared_ptr<double> fResultMean;
   std::vector<ULong64_t> fCounts;
   std::vector<double> fSums;
   std::vector<double> fPartialMeans;
   std::vector<double> fCompensations;

public:
   MeanHelper(const std::shared_ptr<double> &meanVPtr, const unsigned int nSlots);
   MeanHelper(MeanHelper &&) = default;
   MeanHelper(const MeanHelper &) = delete;
   void InitTask(TTreeReader *, unsigned int) {}
   void Exec(unsigned int slot, double v);

   template <typename T, std::enable_if_t<IsDataContainer<T>::value, int> = 0>
   void Exec(unsigned int slot, const T &vs)
   {
      for (auto &&v : vs) {

         fCounts[slot]++;
         // Kahan Sum:
         double y = v - fCompensations[slot];
         double t = fSums[slot] + y;
         fCompensations[slot] = (t - fSums[slot]) - y;
         fSums[slot] = t;
      }
   }

   void Initialize() { /* noop */}

   void Finalize();

   // Helper functions for RMergeableValue
   std::unique_ptr<RMergeableValueBase> GetMergeableValue() const final
   {
      const ULong64_t counts = std::accumulate(fCounts.begin(), fCounts.end(), 0ull);
      return std::make_unique<RMergeableMean>(*fResultMean, counts);
   }

   double &PartialUpdate(unsigned int slot);

   std::string GetActionName() { return "Mean"; }

   MeanHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<double> *>(newResult);
      return MeanHelper(result, fSums.size());
   }
};

class R__CLING_PTRCHECK(off) StdDevHelper : public RActionImpl<StdDevHelper> {
   // Number of subsets of data
   unsigned int fNSlots;
   std::shared_ptr<double> fResultStdDev;
   // Number of element for each slot
   std::vector<ULong64_t> fCounts;
   // Mean of each slot
   std::vector<double> fMeans;
   // Squared distance from the mean
   std::vector<double> fDistancesfromMean;

public:
   StdDevHelper(const std::shared_ptr<double> &meanVPtr, const unsigned int nSlots);
   StdDevHelper(StdDevHelper &&) = default;
   StdDevHelper(const StdDevHelper &) = delete;
   void InitTask(TTreeReader *, unsigned int) {}
   void Exec(unsigned int slot, double v);

   template <typename T, std::enable_if_t<IsDataContainer<T>::value, int> = 0>
   void Exec(unsigned int slot, const T &vs)
   {
      for (auto &&v : vs) {
         Exec(slot, v);
      }
   }

   void Initialize() { /* noop */}

   void Finalize();

   // Helper functions for RMergeableValue
   std::unique_ptr<RMergeableValueBase> GetMergeableValue() const final
   {
      const ULong64_t counts = std::accumulate(fCounts.begin(), fCounts.end(), 0ull);
      const Double_t mean =
         std::inner_product(fMeans.begin(), fMeans.end(), fCounts.begin(), 0.) / static_cast<Double_t>(counts);
      return std::make_unique<RMergeableStdDev>(*fResultStdDev, counts, mean);
   }

   std::string GetActionName() { return "StdDev"; }

   StdDevHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<double> *>(newResult);
      return StdDevHelper(result, fCounts.size());
   }
};

template <typename PrevNodeType>
class R__CLING_PTRCHECK(off) DisplayHelper : public RActionImpl<DisplayHelper<PrevNodeType>> {
private:
   using Display_t = ROOT::RDF::RDisplay;
   std::shared_ptr<Display_t> fDisplayerHelper;
   std::shared_ptr<PrevNodeType> fPrevNode;
   size_t fEntriesToProcess;

public:
   DisplayHelper(size_t nRows, const std::shared_ptr<Display_t> &d, const std::shared_ptr<PrevNodeType> &prevNode)
      : fDisplayerHelper(d), fPrevNode(prevNode), fEntriesToProcess(nRows)
   {
   }
   DisplayHelper(DisplayHelper &&) = default;
   DisplayHelper(const DisplayHelper &) = delete;
   void InitTask(TTreeReader *, unsigned int) {}

   template <typename... Columns>
   void Exec(unsigned int, Columns &... columns)
   {
      if (fEntriesToProcess == 0)
         return;

      fDisplayerHelper->AddRow(columns...);
      --fEntriesToProcess;

      if (fEntriesToProcess == 0) {
         // No more entries to process. Send a one-time signal that this node
         // of the graph is done. It is important that the 'StopProcessing'
         // method is only called once from this helper, otherwise it would seem
         // like more than one operation has completed its work.
         fPrevNode->StopProcessing();
      }
   }

   void Initialize() {}

   void Finalize() {}

   std::string GetActionName() { return "Display"; }
};

template <typename T>
void *GetData(ROOT::VecOps::RVec<T> &v)
{
   return v.data();
}

template <typename T>
void *GetData(T & /*v*/)
{
   return nullptr;
}

template <typename T>
void SetBranchesHelper(TTree *inputTree, TTree &outputTree, const std::string &inName, const std::string &name,
                       TBranch *&branch, void *&branchAddress, T *address, RBranchSet &outputBranches,
                       bool /*isDefine*/, int basketSize)
{
   static TClassRef TBOClRef("TBranchObject");

   TBranch *inputBranch = nullptr;
   if (inputTree) {
      inputBranch = inputTree->GetBranch(inName.c_str());
      if (!inputBranch) // try harder
         inputBranch = inputTree->FindBranch(inName.c_str());
   }

   auto *outputBranch = outputBranches.Get(name);
   if (outputBranch) {
      // the output branch was already created, we just need to (re)set its address
      if (inputBranch && inputBranch->IsA() == TBOClRef) {
         outputBranch->SetAddress(reinterpret_cast<T **>(inputBranch->GetAddress()));
      } else if (outputBranch->IsA() != TBranch::Class()) {
         branchAddress = address;
         outputBranch->SetAddress(&branchAddress);
      } else {
         outputBranch->SetAddress(address);
         branchAddress = address;
      }
      return;
   }

   if (inputBranch) {
      // Respect the original bufsize and splitlevel arguments
      // In particular, by keeping splitlevel equal to 0 if this was the case for `inputBranch`, we avoid
      // writing garbage when unsplit objects cannot be written as split objects (e.g. in case of a polymorphic
      // TObject branch, see https://bit.ly/2EjLMId ).
      // A user-provided basket size value takes precedence.
      const auto bufSize = (basketSize > 0) ? basketSize : inputBranch->GetBasketSize();
      const auto splitLevel = inputBranch->GetSplitLevel();

      if (inputBranch->IsA() == TBOClRef) {
         // Need to pass a pointer to pointer
         outputBranch =
            outputTree.Branch(name.c_str(), reinterpret_cast<T **>(inputBranch->GetAddress()), bufSize, splitLevel);
      } else {
         outputBranch = outputTree.Branch(name.c_str(), address, bufSize, splitLevel);
      }
   } else {
      // Set Custom basket size for new branches.
      const auto buffSize = (basketSize > 0) ? basketSize : (inputBranch ? inputBranch->GetBasketSize() : 32000);
      outputBranch = outputTree.Branch(name.c_str(), address, buffSize);
   }
   outputBranches.Insert(name, outputBranch);
   // This is not an array branch, so we don't register the address of the output branch here
   branch = nullptr;
   branchAddress = nullptr;
}

/// Helper function for SnapshotTTreeHelper and SnapshotTTreeHelperMT. It creates new branches for the output TTree of a
/// Snapshot. This overload is called for columns of type `RVec<T>`. For RDF, these can represent:
/// 1. c-style arrays in ROOT files, so we are sure that there are input trees to which we can ask the correct branch
/// title
/// 2. RVecs coming from a custom column or the input file/data-source
/// 3. vectors coming from ROOT files that are being read as RVecs
/// 4. TClonesArray
///
/// In case of 1., we keep aside the pointer to the branch and the pointer to the input value (in `branch` and
/// `branchAddress`) so we can intercept changes in the address of the input branch and tell the output branch.
template <typename T>
void SetBranchesHelper(TTree *inputTree, TTree &outputTree, const std::string &inName, const std::string &outName,
                       TBranch *&branch, void *&branchAddress, RVec<T> *ab, RBranchSet &outputBranches, bool isDefine,
                       int basketSize)
{
   TBranch *inputBranch = nullptr;
   if (inputTree) {
      inputBranch = inputTree->GetBranch(inName.c_str());
      if (!inputBranch) // try harder
         inputBranch = inputTree->FindBranch(inName.c_str());
   }
   auto *outputBranch = outputBranches.Get(outName);

   // if no backing input branch, we must write out an RVec
   bool mustWriteRVec = (inputBranch == nullptr || isDefine);
   // otherwise, if input branch is TClonesArray, must write out an RVec
   if (!mustWriteRVec && std::string_view(inputBranch->GetClassName()) == "TClonesArray") {
      mustWriteRVec = true;
      Warning("Snapshot",
              "Branch \"%s\" contains TClonesArrays but the type specified to Snapshot was RVec<T>. The branch will "
              "be written out as a RVec instead of a TClonesArray. Specify that the type of the branch is "
              "TClonesArray as a Snapshot template parameter to write out a TClonesArray instead.",
              inName.c_str());
   }
   // otherwise, if input branch is a std::vector or RVec, must write out an RVec
   if (!mustWriteRVec) {
      const auto STLKind = TClassEdit::IsSTLCont(inputBranch->GetClassName());
      if (STLKind == ROOT::ESTLType::kSTLvector || STLKind == ROOT::ESTLType::kROOTRVec)
         mustWriteRVec = true;
   }

   if (mustWriteRVec) {
      // Treat:
      // 2. RVec coming from a custom column or a source
      // 3. RVec coming from a column on disk of type vector (the RVec is adopting the data of that vector)
      // 4. TClonesArray written out as RVec<T>
      if (outputBranch) {
         // needs to be SetObject (not SetAddress) to mimic what happens when this TBranchElement is constructed
         outputBranch->SetObject(ab);
      } else {
         // Set Custom basket size for new branches if specified, otherwise get basket size from input branches
         const auto buffSize = (basketSize > 0) ? basketSize : (inputBranch ? inputBranch->GetBasketSize() : 32000);
         auto *b = outputTree.Branch(outName.c_str(), ab, buffSize);
         outputBranches.Insert(outName, b);
      }
      return;
   }

   // else this must be a C-array, aka case 1.
   auto dataPtr = ab->data();

   if (outputBranch) {
      if (outputBranch->IsA() != TBranch::Class()) {
         branchAddress = dataPtr;
         outputBranch->SetAddress(&branchAddress);
      } else {
         outputBranch->SetAddress(dataPtr);
      }
   } else {
      // must construct the leaflist for the output branch and create the branch in the output tree
      auto *const leaf = static_cast<TLeaf *>(inputBranch->GetListOfLeaves()->UncheckedAt(0));
      const auto bname = leaf->GetName();
      auto *sizeLeaf = leaf->GetLeafCount();
      const auto sizeLeafName = sizeLeaf ? std::string(sizeLeaf->GetName()) : std::to_string(leaf->GetLenStatic());

      if (sizeLeaf && !outputBranches.Get(sizeLeafName)) {
         // The output array branch `bname` has dynamic size stored in leaf `sizeLeafName`, but that leaf has not been
         // added to the output tree yet. However, the size leaf has to be available for the creation of the array
         // branch to be successful. So we create the size leaf here.
         const auto sizeTypeStr = TypeName2ROOTTypeName(sizeLeaf->GetTypeName());
         // Use Original basket size for Existing Branches otherwise use Custom basket Size.
         const auto sizeBufSize = (basketSize > 0) ? basketSize : sizeLeaf->GetBranch()->GetBasketSize();
         // The null branch address is a placeholder. It will be set when SetBranchesHelper is called for `sizeLeafName`
         auto *sizeBranch = outputTree.Branch(sizeLeafName.c_str(), (void *)nullptr,
                                              (sizeLeafName + '/' + sizeTypeStr).c_str(), sizeBufSize);
         outputBranches.Insert(sizeLeafName, sizeBranch);
      }

      const auto btype = leaf->GetTypeName();
      const auto rootbtype = TypeName2ROOTTypeName(btype);
      if (rootbtype == ' ') {
         Warning("Snapshot",
                 "RDataFrame::Snapshot: could not correctly construct a leaflist for C-style array in column %s. This "
                 "column will not be written out.",
                 bname);
      } else {
         const auto leaflist = std::string(bname) + "[" + sizeLeafName + "]/" + rootbtype;
         // Use original basket size for existing branches and new basket size for new branches
         const auto branchBufSize = (basketSize > 0) ? basketSize : inputBranch->GetBasketSize();
         outputBranch = outputTree.Branch(outName.c_str(), dataPtr, leaflist.c_str(), branchBufSize);
         outputBranch->SetTitle(inputBranch->GetTitle());
         outputBranches.Insert(outName, outputBranch);
         branch = outputBranch;
         branchAddress = ab->data();
      }
   }
}

void SetBranchesHelper(TTree *inputTree, TTree &outputTree, RBranchSet &outputBranches, int basketSize,
                       const std::string &inputBranchName, const std::string &outputBranchName,
                       const std::type_info &valueTypeID, void *valueAddress, TBranch *&actionHelperBranchPtr,
                       void *&actionHelperBranchPtrAddress);

/// Ensure that the TTree with the resulting snapshot can be written to the target TFile. This means checking that the
/// TFile can be opened in the mode specified in `opts`, deleting any existing TTrees in case
/// `opts.fOverwriteIfExists = true`, or throwing an error otherwise.
void EnsureValidSnapshotTTreeOutput(const RSnapshotOptions &opts, const std::string &treeName,
                                    const std::string &fileName);

/// Helper object for a single-thread TTree-based Snapshot action
template <typename... ColTypes>
class R__CLING_PTRCHECK(off) SnapshotTTreeHelper : public RActionImpl<SnapshotTTreeHelper<ColTypes...>> {
   std::string fFileName;
   std::string fDirName;
   std::string fTreeName;
   RSnapshotOptions fOptions;
   std::unique_ptr<TFile> fOutputFile;
   std::unique_ptr<TTree> fOutputTree; // must be a ptr because TTrees are not copy/move constructible
   bool fBranchAddressesNeedReset{true};
   ColumnNames_t fInputBranchNames; // This contains the resolved aliases
   ColumnNames_t fOutputBranchNames;
   TTree *fInputTree = nullptr; // Current input tree. Set at initialization time (`InitTask`)
   // TODO we might be able to unify fBranches, fBranchAddresses and fOutputBranches
   std::vector<TBranch *> fBranches; // Addresses of branches in output, non-null only for the ones holding C arrays
   std::vector<void *> fBranchAddresses; // Addresses of objects associated to output branches
   RBranchSet fOutputBranches;
   std::vector<bool> fIsDefine;
   ROOT::Detail::RDF::RLoopManager *fOutputLoopManager;
   ROOT::Detail::RDF::RLoopManager *fInputLoopManager;

public:
   using ColumnTypes_t = TypeList<ColTypes...>;
   SnapshotTTreeHelper(std::string_view filename, std::string_view dirname, std::string_view treename,
                       const ColumnNames_t &vbnames, const ColumnNames_t &bnames, const RSnapshotOptions &options,
                       std::vector<bool> &&isDefine, ROOT::Detail::RDF::RLoopManager *loopManager,
                       ROOT::Detail::RDF::RLoopManager *inputLM)
      : fFileName(filename),
        fDirName(dirname),
        fTreeName(treename),
        fOptions(options),
        fInputBranchNames(vbnames),
        fOutputBranchNames(ReplaceDotWithUnderscore(bnames)),
        fBranches(vbnames.size(), nullptr),
        fBranchAddresses(vbnames.size(), nullptr),
        fIsDefine(std::move(isDefine)),
        fOutputLoopManager(loopManager),
        fInputLoopManager(inputLM)
   {
      EnsureValidSnapshotTTreeOutput(fOptions, fTreeName, fFileName);
   }

   SnapshotTTreeHelper(const SnapshotTTreeHelper &) = delete;
   SnapshotTTreeHelper(SnapshotTTreeHelper &&) = default;
   ~SnapshotTTreeHelper()
   {
      if (!fTreeName.empty() /*not moved from*/ && !fOutputFile /* did not run */ && fOptions.fLazy) {
         const auto fileOpenMode = [&]() {
            TString checkupdate = fOptions.fMode;
            checkupdate.ToLower();
            return checkupdate == "update" ? "updated" : "created";
         }();
         Warning("Snapshot",
                 "A lazy Snapshot action was booked but never triggered. The tree '%s' in output file '%s' was not %s. "
                 "In case it was desired instead, remember to trigger the Snapshot operation, by storing "
                 "its result in a variable and for example calling the GetValue() method on it.",
                 fTreeName.c_str(), fFileName.c_str(), fileOpenMode);
      }
   }

   void InitTask(TTreeReader * /*treeReader*/, unsigned int /* slot */)
   {
      // We ask the input RLoopManager if it has a TTree. We cannot rely on getting this information when constructing
      // this action helper, since the TTree might change e.g. when ChangeSpec is called in-between distributed tasks.
      fInputTree = fInputLoopManager->GetTree();
      fBranchAddressesNeedReset = true;
   }

   void Exec(unsigned int /* slot */, ColTypes &... values)
   {
      using ind_t = std::index_sequence_for<ColTypes...>;
      if (!fBranchAddressesNeedReset) {
         UpdateCArraysPtrs(values..., ind_t{});
      } else {
         SetBranches(values..., ind_t{});
         fBranchAddressesNeedReset = false;
      }
      fOutputTree->Fill();
   }

   template <std::size_t... S>
   void UpdateCArraysPtrs(ColTypes &... values, std::index_sequence<S...> /*dummy*/)
   {
      // This code deals with branches which hold C arrays of variable size. It can happen that the buffers
      // associated to those is re-allocated. As a result the value of the pointer can change therewith
      // leaving associated to the branch of the output tree an invalid pointer.
      // With this code, we set the value of the pointer in the output branch anew when needed.
      // Nota bene: the extra ",0" after the invocation of SetAddress, is because that method returns void and
      // we need an int for the expander list.
      int expander[] = {(fBranches[S] && fBranchAddresses[S] != GetData(values)
                         ? fBranches[S]->SetAddress(GetData(values)),
                         fBranchAddresses[S] = GetData(values), 0 : 0, 0)...,
                        0};
      (void)expander; // avoid unused variable warnings for older compilers such as gcc 4.9
   }

   template <std::size_t... S>
   void SetBranches(ColTypes &... values, std::index_sequence<S...> /*dummy*/)
   {
      // create branches in output tree
      int expander[] = {
         (SetBranchesHelper(fInputTree, *fOutputTree, fInputBranchNames[S], fOutputBranchNames[S], fBranches[S],
                            fBranchAddresses[S], &values, fOutputBranches, fIsDefine[S], fOptions.fBasketSize),
          0)...,
         0};
      fOutputBranches.AssertNoNullBranchAddresses();
      (void)expander; // avoid unused variable warnings for older compilers such as gcc 4.9
   }

   template <std::size_t... S>
   void SetEmptyBranches(TTree *inputTree, TTree &outputTree, std::index_sequence<S...>)
   {
      RBranchSet outputBranches{};
      void *dummyValueAddress{};
      TBranch *dummyTBranchPtr{};
      void *dummyTBranchAddress{};
      // We use the expander trick rather than a fold expression to avoid incurring in the bracket depth limit of clang
      int expander[] = {(SetBranchesHelper(inputTree, outputTree, outputBranches, fOptions.fBasketSize,
                                           fInputBranchNames[S], fOutputBranchNames[S], typeid(ColTypes),
                                           dummyValueAddress, dummyTBranchPtr, dummyTBranchAddress),
                         0)...,
                        0};
      (void)expander;
   }

   void Initialize()
   {
      fOutputFile.reset(
         TFile::Open(fFileName.c_str(), fOptions.fMode.c_str(), /*ftitle=*/"",
                     ROOT::CompressionSettings(fOptions.fCompressionAlgorithm, fOptions.fCompressionLevel)));
      if(!fOutputFile)
         throw std::runtime_error("Snapshot: could not create output file " + fFileName);

      TDirectory *outputDir = fOutputFile.get();
      if (!fDirName.empty()) {
         TString checkupdate = fOptions.fMode;
         checkupdate.ToLower();
         if (checkupdate == "update")
            outputDir = fOutputFile->mkdir(fDirName.c_str(), "", true);  // do not overwrite existing directory
         else
            outputDir = fOutputFile->mkdir(fDirName.c_str());
      }

      fOutputTree =
         std::make_unique<TTree>(fTreeName.c_str(), fTreeName.c_str(), fOptions.fSplitLevel, /*dir=*/outputDir);

      if (fOptions.fAutoFlush)
         fOutputTree->SetAutoFlush(fOptions.fAutoFlush);
   }

   void Finalize()
   {
      assert(fOutputTree != nullptr);
      assert(fOutputFile != nullptr);

      // There were no entries to fill the TTree with (either the input TTree was empty or no event passed after
      // filtering). We have already created an empty TTree, now also create the branches to preserve the schema
      if (fOutputTree->GetEntries() == 0) {
         using ind_t = std::index_sequence_for<ColTypes...>;
         SetEmptyBranches(fInputTree, *fOutputTree, ind_t{});
      }
      // use AutoSave to flush TTree contents because TTree::Write writes in gDirectory, not in fDirectory
      fOutputTree->AutoSave("flushbaskets");
      // must destroy the TTree first, otherwise TFile will delete it too leading to a double delete
      fOutputTree.reset();
      fOutputFile->Close();

      // Now connect the data source to the loop manager so it can be used for further processing
      auto fullTreeName = fDirName.empty() ? fTreeName : fDirName + '/' + fTreeName;
      fOutputLoopManager->SetDataSource(std::make_unique<ROOT::Internal::RDF::RTTreeDS>(fullTreeName, fFileName));
   }

   std::string GetActionName() { return "Snapshot"; }

   ROOT::RDF::SampleCallback_t GetSampleCallback() final
   {
      return [this](unsigned int, const RSampleInfo &) mutable { fBranchAddressesNeedReset = true; };
   }

   /**
    * @brief Create a new SnapshotTTreeHelper with a different output file name
    *
    * @param newName A type-erased string with the output file name
    * @return SnapshotTTreeHelper
    *
    * This MakeNew implementation is tied to the cloning feature of actions
    * of the computation graph. In particular, cloning a Snapshot node usually
    * also involves changing the name of the output file, otherwise the cloned
    * Snapshot would overwrite the same file.
    */
   SnapshotTTreeHelper MakeNew(void *newName, std::string_view /*variation*/ = "nominal")
   {
      const std::string finalName = *reinterpret_cast<const std::string *>(newName);
      return SnapshotTTreeHelper{finalName,
                                 fDirName,
                                 fTreeName,
                                 fInputBranchNames,
                                 fOutputBranchNames,
                                 fOptions,
                                 std::vector<bool>(fIsDefine),
                                 fOutputLoopManager,
                                 fInputLoopManager};
   }
};

/// Helper object for a multi-thread TTree-based Snapshot action
template <typename... ColTypes>
class R__CLING_PTRCHECK(off) SnapshotTTreeHelperMT : public RActionImpl<SnapshotTTreeHelperMT<ColTypes...>> {
   unsigned int fNSlots;
   std::unique_ptr<ROOT::TBufferMerger> fMerger; // must use a ptr because TBufferMerger is not movable
   std::vector<std::shared_ptr<ROOT::TBufferMergerFile>> fOutputFiles;
   std::vector<std::unique_ptr<TTree>> fOutputTrees;
   std::vector<int> fBranchAddressesNeedReset; // vector<bool> does not allow concurrent writing of different elements
   std::string fFileName;           // name of the output file name
   std::string fDirName;            // name of TFile subdirectory in which output must be written (possibly empty)
   std::string fTreeName;           // name of output tree
   RSnapshotOptions fOptions;       // struct holding options to pass down to TFile and TTree in this action
   ColumnNames_t fInputBranchNames; // This contains the resolved aliases
   ColumnNames_t fOutputBranchNames;
   std::vector<TTree *> fInputTrees; // Current input trees. Set at initialization time (`InitTask`)
   // Addresses of branches in output per slot, non-null only for the ones holding C arrays
   std::vector<std::vector<TBranch *>> fBranches;
   // Addresses associated to output branches per slot, non-null only for the ones holding C arrays
   std::vector<std::vector<void *>> fBranchAddresses;
   std::vector<RBranchSet> fOutputBranches;
   std::vector<bool> fIsDefine;
   ROOT::Detail::RDF::RLoopManager *fOutputLoopManager;
   ROOT::Detail::RDF::RLoopManager *fInputLoopManager;
   TFile *fOutputFile; // Non-owning view on the output file

public:
   using ColumnTypes_t = TypeList<ColTypes...>;

   SnapshotTTreeHelperMT(const unsigned int nSlots, std::string_view filename, std::string_view dirname,
                         std::string_view treename, const ColumnNames_t &vbnames, const ColumnNames_t &bnames,
                         const RSnapshotOptions &options, std::vector<bool> &&isDefine,
                         ROOT::Detail::RDF::RLoopManager *loopManager, ROOT::Detail::RDF::RLoopManager *inputLM)
      : fNSlots(nSlots),
        fOutputFiles(fNSlots),
        fOutputTrees(fNSlots),
        fBranchAddressesNeedReset(fNSlots, 1),
        fFileName(filename),
        fDirName(dirname),
        fTreeName(treename),
        fOptions(options),
        fInputBranchNames(vbnames),
        fOutputBranchNames(ReplaceDotWithUnderscore(bnames)),
        fInputTrees(fNSlots),
        fBranches(fNSlots, std::vector<TBranch *>(vbnames.size(), nullptr)),
        fBranchAddresses(fNSlots, std::vector<void *>(vbnames.size(), nullptr)),
        fOutputBranches(fNSlots),
        fIsDefine(std::move(isDefine)),
        fOutputLoopManager(loopManager),
        fInputLoopManager(inputLM)
   {
      EnsureValidSnapshotTTreeOutput(fOptions, fTreeName, fFileName);
   }
   SnapshotTTreeHelperMT(const SnapshotTTreeHelperMT &) = delete;
   SnapshotTTreeHelperMT(SnapshotTTreeHelperMT &&) = default;
   ~SnapshotTTreeHelperMT()
   {
      if (!fTreeName.empty() /*not moved from*/ && fOptions.fLazy && !fOutputFiles.empty() &&
          std::all_of(fOutputFiles.begin(), fOutputFiles.end(), [](const auto &f) { return !f; }) /* never run */) {
         const auto fileOpenMode = [&]() {
            TString checkupdate = fOptions.fMode;
            checkupdate.ToLower();
            return checkupdate == "update" ? "updated" : "created";
         }();
         Warning("Snapshot",
                 "A lazy Snapshot action was booked but never triggered. The tree '%s' in output file '%s' was not %s. "
                 "In case it was desired instead, remember to trigger the Snapshot operation, by storing "
                 "its result in a variable and for example calling the GetValue() method on it.",
                 fTreeName.c_str(), fFileName.c_str(), fileOpenMode);
      }
   }

   void InitTask(TTreeReader *r, unsigned int slot)
   {
      ::TDirectory::TContext c; // do not let tasks change the thread-local gDirectory
      if (!fOutputFiles[slot]) {
         // first time this thread executes something, let's create a TBufferMerger output directory
         fOutputFiles[slot] = fMerger->GetFile();
      }
      TDirectory *treeDirectory = fOutputFiles[slot].get();
      if (!fDirName.empty()) {
         // call returnExistingDirectory=true since MT can end up making this call multiple times
         treeDirectory = fOutputFiles[slot]->mkdir(fDirName.c_str(), "", true);
      }
      // re-create output tree as we need to create its branches again, with new input variables
      // TODO we could instead create the output tree and its branches, change addresses of input variables in each task
      fOutputTrees[slot] =
         std::make_unique<TTree>(fTreeName.c_str(), fTreeName.c_str(), fOptions.fSplitLevel, /*dir=*/treeDirectory);
      fOutputTrees[slot]->SetBit(TTree::kEntriesReshuffled);
      // TODO can be removed when RDF supports interleaved TBB task execution properly, see ROOT-10269
      fOutputTrees[slot]->SetImplicitMT(false);
      if (fOptions.fAutoFlush)
         fOutputTrees[slot]->SetAutoFlush(fOptions.fAutoFlush);
      if (r) {
         // We could be getting a task-local TTreeReader from the TTreeProcessorMT.
         fInputTrees[slot] = r->GetTree();
      } else {
         fInputTrees[slot] = fInputLoopManager->GetTree();
      }
      fBranchAddressesNeedReset[slot] = 1; // reset first event flag for this slot
   }

   void FinalizeTask(unsigned int slot)
   {
      if (fOutputTrees[slot]->GetEntries() > 0)
         fOutputFiles[slot]->Write();
      // clear now to avoid concurrent destruction of output trees and input tree (which has them listed as fClones)
      fOutputTrees[slot].reset(nullptr);
      fOutputBranches[slot].Clear();
   }

   void Exec(unsigned int slot, ColTypes &... values)
   {
      using ind_t = std::index_sequence_for<ColTypes...>;
      if (fBranchAddressesNeedReset[slot] == 0) {
         UpdateCArraysPtrs(slot, values..., ind_t{});
      } else {
         SetBranches(slot, values..., ind_t{});
         fBranchAddressesNeedReset[slot] = 0;
      }
      fOutputTrees[slot]->Fill();
      auto entries = fOutputTrees[slot]->GetEntries();
      auto autoFlush = fOutputTrees[slot]->GetAutoFlush();
      if ((autoFlush > 0) && (entries % autoFlush == 0))
         fOutputFiles[slot]->Write();
   }

   template <std::size_t... S>
   void UpdateCArraysPtrs(unsigned int slot, ColTypes &... values, std::index_sequence<S...> /*dummy*/)
   {
      // This code deals with branches which hold C arrays of variable size. It can happen that the buffers
      // associated to those is re-allocated. As a result the value of the pointer can change therewith
      // leaving associated to the branch of the output tree an invalid pointer.
      // With this code, we set the value of the pointer in the output branch anew when needed.
      // Nota bene: the extra ",0" after the invocation of SetAddress, is because that method returns void and
      // we need an int for the expander list.
      int expander[] = {(fBranches[slot][S] && fBranchAddresses[slot][S] != GetData(values)
                         ? fBranches[slot][S]->SetAddress(GetData(values)),
                         fBranchAddresses[slot][S] = GetData(values), 0 : 0, 0)...,
                        0};
      (void)expander; // avoid unused parameter warnings (gcc 12.1)
      (void)slot; // Also "slot" might be unused, in case "values" is empty
   }

   template <std::size_t... S>
   void SetBranches(unsigned int slot, ColTypes &... values, std::index_sequence<S...> /*dummy*/)
   {
      // hack to call TTree::Branch on all variadic template arguments
      int expander[] = {(SetBranchesHelper(fInputTrees[slot], *fOutputTrees[slot], fInputBranchNames[S],
                                           fOutputBranchNames[S], fBranches[slot][S], fBranchAddresses[slot][S],
                                           &values, fOutputBranches[slot], fIsDefine[S], fOptions.fBasketSize),
                         0)...,
                        0};
      fOutputBranches[slot].AssertNoNullBranchAddresses();
      (void)expander; // avoid unused parameter warnings (gcc 12.1)
   }

   template <std::size_t... S>
   void SetEmptyBranches(TTree *inputTree, TTree &outputTree, std::index_sequence<S...>)
   {
      void *dummyValueAddress{};
      TBranch *dummyTBranchPtr{};
      void *dummyTBranchAddress{};
      RBranchSet outputBranches{};
      // We use the expander trick rather than a fold expression to avoid incurring in the bracket depth limit of clang
      int expander[] = {(SetBranchesHelper(inputTree, outputTree, outputBranches, fOptions.fBasketSize,
                                           fInputBranchNames[S], fOutputBranchNames[S], typeid(ColTypes),
                                           dummyValueAddress, dummyTBranchPtr, dummyTBranchAddress),
                         0)...,
                        0};
      (void)expander;
   }

   void Initialize()
   {
      const auto cs = ROOT::CompressionSettings(fOptions.fCompressionAlgorithm, fOptions.fCompressionLevel);
      auto outFile = std::unique_ptr<TFile>{
         TFile::Open(fFileName.c_str(), fOptions.fMode.c_str(), /*ftitle=*/fFileName.c_str(), cs)};
      if (!outFile)
         throw std::runtime_error("Snapshot: could not create output file " + fFileName);
      fOutputFile = outFile.get();
      fMerger = std::make_unique<ROOT::TBufferMerger>(std::move(outFile));
   }

   void Finalize()
   {
      assert(std::any_of(fOutputFiles.begin(), fOutputFiles.end(), [](const auto &ptr) { return ptr != nullptr; }));

      for (auto &file : fOutputFiles) {
         if (file) {
            file->Write();
            file->Close();
         }
      }

      // If there were no entries to fill the TTree with (either the input TTree was empty or no event passed after
      // filtering), create an empty TTree in the output file and create the branches to preserve the schema
      auto fullTreeName = fDirName.empty() ? fTreeName : fDirName + '/' + fTreeName;
      assert(fOutputFile && "Missing output file in Snapshot finalization.");
      if (!fOutputFile->Get(fullTreeName.c_str())) {

         // First find in which directory we need to write the output TTree
         TDirectory *treeDirectory = fOutputFile;
         if (!fDirName.empty()) {
            treeDirectory = fOutputFile->mkdir(fDirName.c_str(), "", true);
         }
         ::TDirectory::TContext c{treeDirectory};

         // Create the output TTree and create the user-requested branches
         auto outTree =
            std::make_unique<TTree>(fTreeName.c_str(), fTreeName.c_str(), fOptions.fSplitLevel, /*dir=*/treeDirectory);
         using ind_t = std::index_sequence_for<ColTypes...>;
         SetEmptyBranches(fInputLoopManager->GetTree(), *outTree, ind_t{});

         fOutputFile->Write();
      }

      // flush all buffers to disk by destroying the TBufferMerger
      fOutputFiles.clear();
      fMerger.reset();

      // Now connect the data source to the loop manager so it can be used for further processing
      fOutputLoopManager->SetDataSource(std::make_unique<ROOT::Internal::RDF::RTTreeDS>(fullTreeName, fFileName));
   }

   std::string GetActionName() { return "Snapshot"; }

   ROOT::RDF::SampleCallback_t GetSampleCallback() final
   {
      return [this](unsigned int slot, const RSampleInfo &) mutable { fBranchAddressesNeedReset[slot] = 1; };
   }

   /**
    * @brief Create a new SnapshotTTreeHelperMT with a different output file name
    *
    * @param newName A type-erased string with the output file name
    * @return SnapshotTTreeHelperMT
    *
    * This MakeNew implementation is tied to the cloning feature of actions
    * of the computation graph. In particular, cloning a Snapshot node usually
    * also involves changing the name of the output file, otherwise the cloned
    * Snapshot would overwrite the same file.
    */
   SnapshotTTreeHelperMT MakeNew(void *newName, std::string_view /*variation*/ = "nominal")
   {
      const std::string finalName = *reinterpret_cast<const std::string *>(newName);
      return SnapshotTTreeHelperMT{fNSlots,
                                   finalName,
                                   fDirName,
                                   fTreeName,
                                   fInputBranchNames,
                                   fOutputBranchNames,
                                   fOptions,
                                   std::vector<bool>(fIsDefine),
                                   fOutputLoopManager,
                                   fInputLoopManager};
   }
};

/// Ensure that the RNTuple with the resulting snapshot can be written to the target TFile. This means checking that the
/// TFile can be opened in the mode specified in `opts`, deleting any existing RNTuples in case
/// `opts.fOverwriteIfExists = true`, or throwing an error otherwise.
void EnsureValidSnapshotRNTupleOutput(const RSnapshotOptions &opts, const std::string &ntupleName,
                                      const std::string &fileName);

/// Helper function to update the value of an RNTuple's field in the provided entry.
template <typename T>
void SetFieldsHelper(T &value, std::string_view fieldName, ROOT::REntry *entry)
{
   entry->BindRawPtr(fieldName, &value);
}

/// Helper object for a single-thread RNTuple-based Snapshot action
template <typename... ColTypes>
class R__CLING_PTRCHECK(off) SnapshotRNTupleHelper : public RActionImpl<SnapshotRNTupleHelper<ColTypes...>> {
   std::string fFileName;
   std::string fDirName;
   std::string fNTupleName;

   std::unique_ptr<TFile> fOutputFile{nullptr};

   RSnapshotOptions fOptions;
   ROOT::Detail::RDF::RLoopManager *fOutputLoopManager;
   ColumnNames_t fInputFieldNames; // This contains the resolved aliases
   ColumnNames_t fOutputFieldNames;
   std::unique_ptr<ROOT::RNTupleWriter> fWriter{nullptr};

   ROOT::REntry *fOutputEntry;

   std::vector<bool> fIsDefine;

public:
   using ColumnTypes_t = TypeList<ColTypes...>;
   SnapshotRNTupleHelper(std::string_view filename, std::string_view dirname, std::string_view ntuplename,
                         const ColumnNames_t &vfnames, const ColumnNames_t &fnames, const RSnapshotOptions &options,
                         ROOT::Detail::RDF::RLoopManager *lm, std::vector<bool> &&isDefine)
      : fFileName(filename),
        fDirName(dirname),
        fNTupleName(ntuplename),
        fOptions(options),
        fOutputLoopManager(lm),
        fInputFieldNames(vfnames),
        fOutputFieldNames(ReplaceDotWithUnderscore(fnames)),
        fIsDefine(std::move(isDefine))
   {
      EnsureValidSnapshotRNTupleOutput(fOptions, fNTupleName, fFileName);
   }

   SnapshotRNTupleHelper(const SnapshotRNTupleHelper &) = delete;
   SnapshotRNTupleHelper &operator=(const SnapshotRNTupleHelper &) = delete;
   SnapshotRNTupleHelper(SnapshotRNTupleHelper &&) = default;
   SnapshotRNTupleHelper &operator=(SnapshotRNTupleHelper &&) = default;
   ~SnapshotRNTupleHelper()
   {
      if (!fNTupleName.empty() && !fOutputLoopManager->GetDataSource() && fOptions.fLazy)
         Warning("Snapshot", "A lazy Snapshot action was booked but never triggered.");
   }

   void InitTask(TTreeReader *, unsigned int /* slot */) {}

   void Exec(unsigned int /* slot */, ColTypes &...values)
   {
      using ind_t = std::index_sequence_for<ColTypes...>;

      SetFields(values..., ind_t{});
      fWriter->Fill();
   }

   template <std::size_t... S>
   void SetFields(ColTypes &...values, std::index_sequence<S...> /*dummy*/)
   {
      int expander[] = {(SetFieldsHelper(values, fOutputFieldNames[S], fOutputEntry), 0)..., 0};
      (void)expander; // avoid unused variable warnings for older compilers (gcc 14.1)
   }

   void Initialize()
   {
      using ind_t = std::index_sequence_for<ColTypes...>;

      auto model = ROOT::RNTupleModel::Create();
      MakeFields(*model, ind_t{});
      fOutputEntry = &model->GetDefaultEntry();

      ROOT::RNTupleWriteOptions writeOptions;
      writeOptions.SetCompression(fOptions.fCompressionAlgorithm, fOptions.fCompressionLevel);

      fOutputFile.reset(TFile::Open(fFileName.c_str(), fOptions.fMode.c_str()));
      if (!fOutputFile)
         throw std::runtime_error("Snapshot: could not create output file " + fFileName);

      TDirectory *outputDir = fOutputFile.get();
      if (!fDirName.empty()) {
         TString checkupdate = fOptions.fMode;
         checkupdate.ToLower();
         if (checkupdate == "update")
            outputDir = fOutputFile->mkdir(fDirName.c_str(), "", true); // do not overwrite existing directory
         else
            outputDir = fOutputFile->mkdir(fDirName.c_str());
      }

      fWriter = ROOT::RNTupleWriter::Append(std::move(model), fNTupleName, *outputDir, writeOptions);
   }

   template <std::size_t... S>
   void MakeFields(ROOT::RNTupleModel &model, std::index_sequence<S...> /*dummy*/)
   {
      int expander[] = {(model.MakeField<ColTypes>(fOutputFieldNames[S]), 0)..., 0};
      (void)expander; // avoid unused variable warnings for older compilers (gcc 14.1)
   }

   void Finalize()
   {
      fWriter.reset();
      // We can now set the data source of the loop manager for the RDataFrame that is returned by the Snapshot call.
      fOutputLoopManager->SetDataSource(
         std::make_unique<ROOT::RDF::RNTupleDS>(fDirName + "/" + fNTupleName, fFileName));
   }

   std::string GetActionName() { return "Snapshot"; }

   ROOT::RDF::SampleCallback_t GetSampleCallback() final
   {
      return [](unsigned int, const RSampleInfo &) mutable {};
   }

   /**
    * @brief Create a new SnapshotRNTupleHelper with a different output file name
    *
    * @param newName A type-erased string with the output file name
    * @return SnapshotRNTupleHelper
    *
    * This MakeNew implementation is tied to the cloning feature of actions
    * of the computation graph. In particular, cloning a Snapshot node usually
    * also involves changing the name of the output file, otherwise the cloned
    * Snapshot would overwrite the same file.
    */
   SnapshotRNTupleHelper MakeNew(void *newName)
   {
      const std::string finalName = *reinterpret_cast<const std::string *>(newName);
      return SnapshotRNTupleHelper{finalName,
                                   fNTupleName,
                                   fInputFieldNames,
                                   fOutputFieldNames,
                                   fOptions,
                                   fOutputLoopManager,
                                   std::vector<bool>(fIsDefine)};
   }
};

class R__CLING_PTRCHECK(off) UntypedSnapshotRNTupleHelper final : public RActionImpl<UntypedSnapshotRNTupleHelper> {
   std::string fFileName;
   std::string fDirName;
   std::string fNTupleName;

   std::unique_ptr<TFile> fOutputFile{nullptr};

   RSnapshotOptions fOptions;
   ROOT::Detail::RDF::RLoopManager *fInputLoopManager;
   ROOT::Detail::RDF::RLoopManager *fOutputLoopManager;
   ColumnNames_t fInputFieldNames; // This contains the resolved aliases
   ColumnNames_t fOutputFieldNames;
   std::unique_ptr<ROOT::RNTupleWriter> fWriter{nullptr};

   ROOT::REntry *fOutputEntry;

   std::vector<bool> fIsDefine;

   std::vector<const std::type_info *> fInputColumnTypeIDs; // Types for the input columns

public:
   UntypedSnapshotRNTupleHelper(std::string_view filename, std::string_view dirname, std::string_view ntuplename,
                                const ColumnNames_t &vfnames, const ColumnNames_t &fnames,
                                const RSnapshotOptions &options, ROOT::Detail::RDF::RLoopManager *inputLM,
                                ROOT::Detail::RDF::RLoopManager *outputLM, std::vector<bool> &&isDefine,
                                const std::vector<const std::type_info *> &colTypeIDs);

   UntypedSnapshotRNTupleHelper(const UntypedSnapshotRNTupleHelper &) = delete;
   UntypedSnapshotRNTupleHelper &operator=(const UntypedSnapshotRNTupleHelper &) = delete;
   UntypedSnapshotRNTupleHelper(UntypedSnapshotRNTupleHelper &&) = default;
   UntypedSnapshotRNTupleHelper &operator=(UntypedSnapshotRNTupleHelper &&) = default;
   ~UntypedSnapshotRNTupleHelper() final;

   void InitTask(TTreeReader *, unsigned int /* slot */) {}

   void Exec(unsigned int /* slot */, const std::vector<void *> &values);

   void Initialize();

   void Finalize();

   std::string GetActionName() { return "Snapshot"; }

   ROOT::RDF::SampleCallback_t GetSampleCallback() final
   {
      return [](unsigned int, const RSampleInfo &) mutable {};
   }

   UntypedSnapshotRNTupleHelper MakeNew(void *newName);
};

class R__CLING_PTRCHECK(off) UntypedSnapshotTTreeHelper final : public RActionImpl<UntypedSnapshotTTreeHelper> {
   std::string fFileName;
   std::string fDirName;
   std::string fTreeName;
   RSnapshotOptions fOptions;
   std::unique_ptr<TFile> fOutputFile;
   std::unique_ptr<TTree> fOutputTree; // must be a ptr because TTrees are not copy/move constructible
   bool fBranchAddressesNeedReset{true};
   ColumnNames_t fInputBranchNames; // This contains the resolved aliases
   ColumnNames_t fOutputBranchNames;
   TTree *fInputTree = nullptr; // Current input tree. Set at initialization time (`InitTask`)
   // TODO we might be able to unify fBranches, fBranchAddresses and fOutputBranches
   std::vector<TBranch *> fBranches;     // Addresses of branches in output, non-null only for the ones holding C arrays
   std::vector<void *> fBranchAddresses; // Addresses of objects associated to output branches
   RBranchSet fOutputBranches;
   std::vector<bool> fIsDefine;
   ROOT::Detail::RDF::RLoopManager *fOutputLoopManager;
   ROOT::Detail::RDF::RLoopManager *fInputLoopManager;
   std::vector<const std::type_info *> fInputColumnTypeIDs; // Types for the input columns

public:
   UntypedSnapshotTTreeHelper(std::string_view filename, std::string_view dirname, std::string_view treename,
                              const ColumnNames_t &vbnames, const ColumnNames_t &bnames,
                              const RSnapshotOptions &options, std::vector<bool> &&isDefine,
                              ROOT::Detail::RDF::RLoopManager *loopManager, ROOT::Detail::RDF::RLoopManager *inputLM,
                              const std::vector<const std::type_info *> &colTypeIDs);

   UntypedSnapshotTTreeHelper(const UntypedSnapshotTTreeHelper &) = delete;
   UntypedSnapshotTTreeHelper &operator=(const UntypedSnapshotTTreeHelper &) = delete;
   UntypedSnapshotTTreeHelper(UntypedSnapshotTTreeHelper &&) = default;
   UntypedSnapshotTTreeHelper &operator=(UntypedSnapshotTTreeHelper &&) = default;
   ~UntypedSnapshotTTreeHelper() final;

   void InitTask(TTreeReader *, unsigned int);

   void Exec(unsigned int, const std::vector<void *> &values);

   void UpdateCArraysPtrs(const std::vector<void *> &values);

   void SetBranches(const std::vector<void *> &values);

   void SetEmptyBranches(TTree *inputTree, TTree &outputTree);

   void Initialize();

   void Finalize();

   std::string GetActionName() { return "Snapshot"; }

   ROOT::RDF::SampleCallback_t GetSampleCallback() final
   {
      return [this](unsigned int, const RSampleInfo &) mutable { fBranchAddressesNeedReset = true; };
   }

   UntypedSnapshotTTreeHelper MakeNew(void *newName, std::string_view /*variation*/ = "nominal");
};

class R__CLING_PTRCHECK(off) UntypedSnapshotTTreeHelperMT final : public RActionImpl<UntypedSnapshotTTreeHelperMT> {

   // IMT-specific data members

   unsigned int fNSlots;
   std::unique_ptr<ROOT::TBufferMerger> fMerger; // must use a ptr because TBufferMerger is not movable
   std::vector<std::shared_ptr<ROOT::TBufferMergerFile>> fOutputFiles;
   std::vector<std::unique_ptr<TTree>> fOutputTrees;
   std::vector<int> fBranchAddressesNeedReset; // vector<bool> does not allow concurrent writing of different elements
   std::vector<TTree *> fInputTrees; // Current input trees, one per slot. Set at initialization time (`InitTask`)
   // Addresses of branches in output per slot, non-null only for the ones holding C arrays
   std::vector<std::vector<TBranch *>> fBranches;
   // Addresses of objects associated to output branches per slot, non-null only for the ones holding C arrays
   std::vector<std::vector<void *>> fBranchAddresses;
   std::vector<RBranchSet> fOutputBranches; // Unique set of output branches, one per slot.

   // Attributes of the output TTree

   std::string fFileName;
   std::string fDirName;
   std::string fTreeName;
   TFile *fOutputFile; // Non-owning view on the output file
   RSnapshotOptions fOptions;
   std::vector<std::string> fOutputBranchNames;

   // Attributes related to the computation graph

   ROOT::Detail::RDF::RLoopManager *fOutputLoopManager;
   ROOT::Detail::RDF::RLoopManager *fInputLoopManager;
   std::vector<std::string> fInputBranchNames;              // This contains the resolved aliases
   std::vector<const std::type_info *> fInputColumnTypeIDs; // Types for the input columns

   std::vector<bool> fIsDefine;

public:
   UntypedSnapshotTTreeHelperMT(unsigned int nSlots, std::string_view filename, std::string_view dirname,
                                std::string_view treename, const ColumnNames_t &vbnames, const ColumnNames_t &bnames,
                                const RSnapshotOptions &options, std::vector<bool> &&isDefine,
                                ROOT::Detail::RDF::RLoopManager *loopManager, ROOT::Detail::RDF::RLoopManager *inputLM,
                                const std::vector<const std::type_info *> &colTypeIDs);

   UntypedSnapshotTTreeHelperMT(const UntypedSnapshotTTreeHelperMT &) = delete;
   UntypedSnapshotTTreeHelperMT &operator=(const UntypedSnapshotTTreeHelperMT &) = delete;
   UntypedSnapshotTTreeHelperMT(UntypedSnapshotTTreeHelperMT &&) = default;
   UntypedSnapshotTTreeHelperMT &operator=(UntypedSnapshotTTreeHelperMT &&) = default;
   ~UntypedSnapshotTTreeHelperMT() final;

   void InitTask(TTreeReader *r, unsigned int slot);

   void FinalizeTask(unsigned int slot);

   void Exec(unsigned int slot, const std::vector<void *> &values);

   void UpdateCArraysPtrs(unsigned int slot, const std::vector<void *> &values);

   void SetBranches(unsigned int slot, const std::vector<void *> &values);

   void SetEmptyBranches(TTree *inputTree, TTree &outputTree);

   void Initialize();

   void Finalize();

   std::string GetActionName() { return "Snapshot"; }

   ROOT::RDF::SampleCallback_t GetSampleCallback() final
   {
      return [this](unsigned int slot, const RSampleInfo &) mutable { fBranchAddressesNeedReset[slot] = 1; };
   }

   UntypedSnapshotTTreeHelperMT MakeNew(void *newName, std::string_view /*variation*/ = "nominal");
};

template <typename Acc, typename Merge, typename R, typename T, typename U,
          bool MustCopyAssign = std::is_same<R, U>::value>
class R__CLING_PTRCHECK(off) AggregateHelper
   : public RActionImpl<AggregateHelper<Acc, Merge, R, T, U, MustCopyAssign>> {
   Acc fAggregate;
   Merge fMerge;
   std::shared_ptr<U> fResult;
   Results<U> fAggregators;

public:
   using ColumnTypes_t = TypeList<T>;

   AggregateHelper(Acc &&f, Merge &&m, const std::shared_ptr<U> &result, const unsigned int nSlots)
      : fAggregate(std::move(f)), fMerge(std::move(m)), fResult(result), fAggregators(nSlots, *result)
   {
   }

   AggregateHelper(Acc &f, Merge &m, const std::shared_ptr<U> &result, const unsigned int nSlots)
      : fAggregate(f), fMerge(m), fResult(result), fAggregators(nSlots, *result)
   {
   }

   AggregateHelper(AggregateHelper &&) = default;
   AggregateHelper(const AggregateHelper &) = delete;

   void InitTask(TTreeReader *, unsigned int) {}

   template <bool MustCopyAssign_ = MustCopyAssign, std::enable_if_t<MustCopyAssign_, int> = 0>
   void Exec(unsigned int slot, const T &value)
   {
      fAggregators[slot] = fAggregate(fAggregators[slot], value);
   }

   template <bool MustCopyAssign_ = MustCopyAssign, std::enable_if_t<!MustCopyAssign_, int> = 0>
   void Exec(unsigned int slot, const T &value)
   {
      fAggregate(fAggregators[slot], value);
   }

   void Initialize() { /* noop */}

   template <typename MergeRet = typename CallableTraits<Merge>::ret_type,
             bool MergeAll = std::is_same<void, MergeRet>::value>
   std::enable_if_t<MergeAll, void> Finalize()
   {
      fMerge(fAggregators);
      *fResult = fAggregators[0];
   }

   template <typename MergeRet = typename CallableTraits<Merge>::ret_type,
             bool MergeTwoByTwo = std::is_same<U, MergeRet>::value>
   std::enable_if_t<MergeTwoByTwo, void> Finalize(...) // ... needed to let compiler distinguish overloads
   {
      for (const auto &acc : fAggregators)
         *fResult = fMerge(*fResult, acc);
   }

   U &PartialUpdate(unsigned int slot) { return fAggregators[slot]; }

   std::string GetActionName() { return "Aggregate"; }

   AggregateHelper MakeNew(void *newResult, std::string_view /*variation*/ = "nominal")
   {
      auto &result = *static_cast<std::shared_ptr<U> *>(newResult);
      return AggregateHelper(fAggregate, fMerge, result, fAggregators.size());
   }
};

} // end of NS RDF
} // end of NS Internal
} // end of NS ROOT

/// \endcond

#endif
