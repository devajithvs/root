// @(#)root/hist:$Id$
// Author: Maciej Zimnoch   30/09/2013

/*************************************************************************
 * Copyright (C) 1995-2013, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/
#ifndef ROOT_TFormula
#define ROOT_TFormula


#include "TNamed.h"
#include "TBits.h"
#include "TInterpreter.h"
#include "TMath.h"
#include <Math/Types.h>

#include <atomic>
#include <cassert>
#include <list>
#include <map>
#include <string>
#include <vector>

class TMethodCall;


class TFormulaFunction
{
public:
   TString  fName;
   TString  fBody;
   Int_t    fNargs;
   Bool_t   fFound;
   Bool_t   fFuncCall;
   const char *  GetName() const    { return fName.Data(); }
   const char *  GetBody() const    { return fBody.Data(); }
   Int_t    GetNargs() const   { return fNargs;}
   Bool_t   IsFuncCall() const { return fFuncCall;}
   TFormulaFunction(){}
   TFormulaFunction(const TString &name, const TString &body, int numArgs)
      : fName(name),fBody(body),fNargs(numArgs),fFound(false),fFuncCall(true) {}
   TFormulaFunction(const TString& name)
   : fName(name),fBody(""),fNargs(0),fFound(false),fFuncCall(false){}
   Bool_t operator<(const TFormulaFunction &rhv) const
   {
      // order by length - first the longer ones to avoid replacing wrong functions
      if ( fName.Length() < rhv.fName.Length() )
         return true;
      else if ( fName.Length() > rhv.fName.Length() )
         return false;
      // case of equal length
      return fName < rhv.fName && fBody < rhv.fBody;
   }
   Bool_t operator==(const TFormulaFunction &rhv) const
   {
      return fName == rhv.fName && fBody == rhv.fBody && fNargs == rhv.fNargs;
   }
};

class TFormulaVariable
{
public:
   TString fName;
   Double_t fValue;
   Int_t fArrayPos;
   Bool_t fFound;
   const char * GetName() const     { return fName.Data(); }
   Double_t GetInitialValue() const    { return fValue; }
   Int_t    GetArrayPos() const { return fArrayPos; }
   TFormulaVariable():fName(""),fValue(-1),fArrayPos(-1),fFound(false){}
   TFormulaVariable(const TString &name, Double_t value, Int_t pos)
   : fName(name), fValue(value), fArrayPos(pos),fFound(false) {}
   Bool_t operator<(const TFormulaVariable &rhv) const
   {
      return fName < rhv.fName;
   }
};

struct TFormulaParamOrder {
   bool operator() (const TString& a, const TString& b) const;
};


class TFormula : public TNamed
{
private:

// All data members are transient apart from the string defining the formula and the parameter values
   TString           fClingInput;                  ///<! Input function passed to Cling
   std::vector<Double_t>  fClingVariables;         ///<! Cached variables
   std::vector<Double_t>  fClingParameters;        ///<  Parameter values
   Bool_t            fReadyToExecute;              ///<! Transient to force initialization
   std::atomic<Bool_t>  fClingInitialized;         ///<! Transient to force re-initialization
   Bool_t            fAllParametersSetted;         ///<  Flag to control if all parameters are setted
   Bool_t            fLazyInitialization = kFALSE; ///<! Transient flag to control lazy initialization (needed for reading from files)
   std::unique_ptr<TMethodCall> fMethod;           ///<! Pointer to methodcall
   TString           fClingName;                   ///<! Unique name passed to Cling to define the function ( double clingName(double*x, double*p) )
   std::string       fSavedInputFormula;           ///<! Unique name used to defined the function and used in the global map (need to be saved in case of lazy initialization)

   using CallFuncSignature = TInterpreter::CallFuncIFacePtr_t::Generic_t;
   std::string       fGradGenerationInput;         ///<! Input query to clad to generate a gradient
   std::string       fHessGenerationInput;         ///<! Input query to clad to generate a hessian
   CallFuncSignature fFuncPtr = nullptr;           ///<! Function pointer, owned by the JIT.
   CallFuncSignature fGradFuncPtr = nullptr;       ///<! Function pointer, owned by the JIT.
   CallFuncSignature fHessFuncPtr = nullptr;       ///<! Function pointer, owned by the JIT.
   void *   fLambdaPtr = nullptr;                  ///<! Pointer to the lambda function
   static bool       fIsCladRuntimeIncluded;

   void     InputFormulaIntoCling();
   Bool_t   PrepareEvalMethod();
   void     FillDefaults();
   void     HandlePolN(TString &formula);
   void     HandleParametrizedFunctions(TString &formula);
   void HandleParamRanges(TString &formula);
   void HandleFunctionArguments(TString &formula);
   void     HandleExponentiation(TString &formula);
   void     HandleLinear(TString &formula);
   Bool_t   InitLambdaExpression(const char * formula);
   static Bool_t   IsDefaultVariableName(const TString &name);
   void ReplaceAllNames(TString &formula, std::map<TString, TString> &substitutions);
   void FillParametrizedFunctions(std::map<std::pair<TString, Int_t>, std::pair<TString, TString>> &functions);
   void FillVecFunctionsShurtCuts();
   void ReInitializeEvalMethod();
   std::string GetGradientFuncName() const {
      return std::string(GetUniqueFuncName().Data()) + "_grad_1";
   }
   std::string GetHessianFuncName() const {
      return std::string(GetUniqueFuncName().Data()) + "_hessian_1";
   }
   bool HasGradientGenerationFailed() const {
      return !fGradFuncPtr && !fGradGenerationInput.empty();
   }
   bool HasHessianGenerationFailed() const {
      return !fHessFuncPtr && !fHessGenerationInput.empty();
   }

protected:

   std::list<TFormulaFunction>         fFuncs;              ///<!
   std::map<TString,TFormulaVariable>  fVars;               ///<!  List of  variable names
   std::map<TString,Int_t,TFormulaParamOrder>   fParams;    ///<|| List of  parameter names
   std::map<TString,Double_t>          fConsts;             ///<!
   std::map<TString,TString>           fFunctionsShortcuts; ///<!
   TString                             fFormula;            ///<   String representing the formula expression
   Int_t                               fNdim;               ///<   Dimension - needed for lambda expressions
   Int_t                               fNpar;               ///<!  Number of parameter (transient since we save the vector)
   Int_t                               fNumber;             ///<   Number used to identify pre-defined functions (gaus, expo,..)
   std::vector<TObject*>               fLinearParts;        ///<   Vector of linear functions
   Bool_t                              fVectorized = false; ///<   Whether we should use vectorized or regular variables
   // (we default to false since a lot of functions still cannot be expressed in vectorized form)

   static Bool_t IsOperator(const char c);
   static Bool_t IsBracket(const char c);
   static Bool_t IsFunctionNameChar(const char c);
   static Bool_t IsScientificNotation(const TString & formula, int ipos);
   static Bool_t IsHexadecimal(const TString & formula, int ipos);
   static Bool_t IsAParameterName(const TString & formula, int ipos);
   void   ExtractFunctors(TString &formula);
   void   PreProcessFormula(TString &formula);
   void   ProcessFormula(TString &formula);
   Bool_t PrepareFormula(TString &formula);
   void   ReplaceParamName(TString &formula, const TString & oldname, const TString & name);
   void   DoAddParameter(const TString &name, Double_t value, bool processFormula);
   void   DoSetParameters(const Double_t * p, Int_t size);
   void   SetPredefinedParamNames();

   Double_t       DoEval(const Double_t * x, const Double_t * p = nullptr) const;
#ifdef R__HAS_VECCORE
   ROOT::Double_v DoEvalVec(const ROOT::Double_v *x, const Double_t *p = nullptr) const;
#endif

public:

   enum EStatusBits {
      kNotGlobal     = BIT(10),    ///< Don't store in gROOT->GetListOfFunction (it should be protected)
      kNormalized    = BIT(14),    ///< Set to true if the TFormula (ex gausn) is normalized
      kLinear        = BIT(16),    ///< Set to true if the TFormula is for linear fitting
      kLambda        = BIT(17)     ///< Set to true if TFormula has been build with a lambda
   };
   using CladStorage = std::vector<Double_t>;

                  TFormula();
          ~TFormula() override;
   TFormula&      operator=(const TFormula &rhs);
   TFormula(const char *name, const char * formula = "", bool addToGlobList = true, bool vectorize = false);
   TFormula(const char *name, const char * formula, int ndim, int npar, bool addToGlobList = true);
                  TFormula(const TFormula &formula);
   //               TFormula(const char *name, Int_t nparams, Int_t ndims);

   void           AddParameter(const TString &name, Double_t value = 0) { DoAddParameter(name,value,true); }
   void           AddVariable(const TString &name, Double_t value = 0);
   void           AddVariables(const TString *vars, const Int_t size);
   Int_t          Compile(const char *expression="");
   void   Copy(TObject &f1) const override;
   void   Clear(Option_t * option="") override;
   template <typename... Args>
   Double_t       Eval(Args... args) const;
   Double_t       EvalPar(const Double_t *x, const Double_t *params = nullptr) const;

   /// Generate gradient computation routine with respect to the parameters.
   /// \returns true if a gradient was generated and GradientPar can be called.
   bool GenerateGradientPar();

   /// Generate hessian computation routine with respect to the parameters.
   /// \returns true if a hessian was generated and HessianPar can be called.
   bool GenerateHessianPar();

   /// Compute the gradient employing automatic differentiation.
   ///
   /// \param[in] x - The given variables, if nullptr the already stored
   ///                variables are used.
   /// \param[out] result - The result of the computation wrt each direction.
   void GradientPar(const Double_t *x, TFormula::CladStorage& result);

   void GradientPar(const Double_t *x, Double_t *result);

   /// Compute the gradient employing automatic differentiation.
   ///
   /// \param[in] x - The given variables, if nullptr the already stored
   ///                variables are used.
   /// \param[out] result - The 2D hessian matrix flattened to form a vector
   ///                      in row-major order.
   void HessianPar(const Double_t *x, TFormula::CladStorage& result);

   void HessianPar(const Double_t *x, Double_t *result);

   // query if TFormula provides gradient computation using AD (CLAD)
   bool HasGeneratedGradient() const {
      return fGradFuncPtr != nullptr;
   }

   // query if TFormula provides hessian computation using AD (CLAD)
   bool HasGeneratedHessian() const {
      return fHessFuncPtr != nullptr;
   }

   // template <class T>
   // T Eval(T x, T y = 0, T z = 0, T t = 0) const;
   template <class T>
   T EvalPar(const T *x, const Double_t *params = nullptr) const {
      return  EvalParVec(x, params);
   }
#ifdef R__HAS_VECCORE
   ROOT::Double_v EvalParVec(const ROOT::Double_v *x, const Double_t *params = nullptr) const;
#endif
   TString        GetExpFormula(Option_t *option = "", const char *fl_format = "%g") const;
   TString        GetGradientFormula() const;
   TString        GetHessianFormula() const;
   TString        GetUniqueFuncName() const {
      assert(fClingName.Length() && "TFormula is not initialized yet!");
      return fClingName;
   }

   const TObject *GetLinearPart(Int_t i) const;
   Int_t          GetNdim() const {return fNdim;}
   Int_t          GetNpar() const {return fNpar;}
   Int_t          GetNumber() const { return fNumber; }
   const char *   GetParName(Int_t ipar) const;
   Int_t          GetParNumber(const char * name) const;
   Double_t       GetParameter(const char * name) const;
   Double_t       GetParameter(Int_t param) const;
   Double_t*      GetParameters() const;
   void           GetParameters(Double_t *params) const;
   Double_t       GetVariable(const char *name) const;
   Int_t          GetVarNumber(const char *name) const;
   TString        GetVarName(Int_t ivar) const;
   Bool_t         IsValid() const { return fReadyToExecute && fClingInitialized; }
   Bool_t IsVectorized() const { return fVectorized; }
   Bool_t         IsLinear() const { return TestBit(kLinear); }
   void           Print(Option_t *option = "") const override;
   void           SetName(const char* name) override;
   void           SetParameter(const char* name, Double_t value);
   void           SetParameter(Int_t param, Double_t value);
   void           SetParameters(const Double_t *params);
   //void           SetParameters(const pair<TString,Double_t> *params, const Int_t size);
   template <typename... Args>
   void           SetParameters(Double_t arg1, Args &&... args);
   void           SetParName(Int_t ipar, const char *name);
   template <typename... Args>
   void           SetParNames(Args &&... args);
   void           SetVariable(const TString &name, Double_t value);
   void           SetVariables(const std::pair<TString,Double_t> *vars, const Int_t size);
   void SetVectorized(Bool_t vectorized);

   ClassDefOverride(TFormula,14)
};

////////////////////////////////////////////////////////////////////////////////
/// Set a list of parameters.
/// The order is by default the alphabetic order given to the parameters,
/// apart if the users has defined explicitly the parameter names.
/// NaN values will be skipped, meaning that the corresponding parameters will not be changed.

template <typename... Args>
void TFormula::SetParameters(Double_t arg1, Args &&...args)
{
   int i = 0;
   for (double val : {arg1, static_cast<Double_t>(args)...}) {
      if(!TMath::IsNaN(val)) SetParameter(i++, val);
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Set parameter names.
/// Empty strings will be skipped, meaning that the corresponding name will not be changed.
template <typename... Args>
void TFormula::SetParNames(Args &&...args)
{
   int i = 0;
   for (auto name : {static_cast<std::string const&>(args)...}) {
      if(!name.empty()) SetParName(i++, name.c_str());
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Set first 1, 2, 3 or 4 variables (e.g. x, y, z and t)
/// and evaluate formula.

template <typename... Args>
Double_t TFormula::Eval(Args... args) const
{
   if (sizeof...(args) > 4) {
      Error("Eval", "Eval() only support setting up to 4 variables");
   }
   double xxx[] = {static_cast<Double_t>(args)...};
   return EvalPar(xxx, nullptr);
}

#endif
