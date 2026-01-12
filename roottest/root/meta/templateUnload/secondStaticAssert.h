#ifndef secondStaticAssert_h
#define secondStaticAssert_h

#include <type_traits>

template <typename BinContentType>
class S final {
public:
   S() {}
   void Scale(double factor)
   {
      // This should fail for integral types
      static_assert(!std::is_integral_v<BinContentType>, "scaling is not supported for integral bin content types");
   }
};
#endif // secondStaticAssert_h
