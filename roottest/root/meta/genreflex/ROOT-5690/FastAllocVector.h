// This is a Mock extracted from LHCb code
//--------------------------------------------------------------------------------
/** @file FastAllocVector.h
 *
 *  Header file for vector class LHCb::FastAllocVector
 *
 *  @author Chris Jones  Christopher.Rob.Jones@cern.ch
 *  @date   29/03/2007
 */
//--------------------------------------------------------------------------------

#ifndef KERNEL_FastAllocVector_H
#define KERNEL_FastAllocVector_H 1

#define XSTR(x) STR(x)
#define STR(x) #x

typedef double         Double32_t;

// namespace blah {
//   template<typename T>
//   class __pool_base {};
  
//   template<bool _Thread>
//   class __pool : public __pool_base<Double32_t> {};
//   template<template <bool> class _PoolTp, typename T>
//   struct __common_pool_policy {};
//   template<typename _Tp, typename _Poolp = __common_pool_policy<__pool, Double32_t> >
//   class __mt_alloc {};
// }


typedef double Double32_t;

namespace blah {
  template<typename D> struct __pool_base {};

  // Note the extra defaulted parameter D = Double32_t
  template<bool _Thread, typename D = Double32_t>
  struct __pool : __pool_base<D> {
    using element_type = D; // expose it so tooling can “see” Double32_t
  };

  // Must stay exactly this shape:
  template<typename T, template <bool, typename D> class _PoolTp>
  struct __common_pool_policy {
    using pool_true     = _PoolTp<true, Double32_t>;                 // forces an instantiation
    using element_type  = typename pool_true::element_type; // pulls Double32_t into the AST
  };

  // Must stay exactly this shape:
  template<typename _Poolp>
  struct __common_pool_policy2 {
   //  using pool_true     = _PoolTp<true, Double32_t>;                 // forces an instantiation
   //  using element_type  = typename pool_true::element_type; // pulls Double32_t into the AST
  };


  template<typename _Tp,
           typename _Poolp = __common_pool_policy2<__common_pool_policy<Double32_t, __pool>> >
  struct __mt_alloc {};
}

namespace LHCb {
  template <typename TYPE,
            typename ALLOC = blah::__mt_alloc<TYPE>>
  struct FastAllocVector {};

  // Example that threads the policy carrying Double32_t
  using VecD32 =
      FastAllocVector<int,
        blah::__mt_alloc<
          int,
          blah::__common_pool_policy2<blah::__common_pool_policy<Double32_t, blah::__pool>>
        >
      >;
}

// namespace LHCb {
//   template < typename TYPE, typename ALLOC = blah::__mt_alloc< TYPE > >
//   class FastAllocVector {};

// }

#endif // KERNEL_FastAllocVector_H
