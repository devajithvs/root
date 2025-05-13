#include <CL/sycl.hpp>
#include <iostream>
using namespace cl;

int syclbasic() {
  sycl::queue q{sycl::cpu_selector_v}; // Only openMP CPU backend is supported right now
  std::cout << "Running on: " << q.get_device().get_info<sycl::info::device::name>() << "\n";
  return 0;
}

