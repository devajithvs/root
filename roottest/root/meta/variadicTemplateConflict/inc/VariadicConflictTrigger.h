namespace edm { template <class T> struct Wrapper {}; }

template <int A=64, bool E=false> struct TestSoALayout {};
template <int A=64, bool E=false> struct TestSoALayout2 {};
template <int A=64, bool E=false> struct TestSoALayout3 {};

template <typename T0, typename... Ts>
struct PortableHostMultiCollection {};

using TestSoA  = TestSoALayout<>;
using TestSoA2 = TestSoALayout2<>;
using TestSoA3 = TestSoALayout3<>;

namespace portabletest {
  using TestHostMultiCollection2 = PortableHostMultiCollection<TestSoA, TestSoA2>;
  using TestHostMultiCollection3 = PortableHostMultiCollection<TestSoA, TestSoA2, TestSoA3>;
}
