#include <sycl/sycl.hpp>
#include <iostream>

void syclvectoradd()
{
   sycl::queue q{sycl::cpu_selector_v};
   const size_t N = 16;

   float *a = sycl::malloc_shared<float>(N, q);

   auto kernel = [=](sycl::id<1> i) {
    size_t idx = i[0];
    a[idx] = static_cast<float>(idx);
    };
    q.parallel_for(sycl::range<1>(N), kernel).wait();

   for (size_t i = 0; i < N; ++i)
      std::cout << "a[" << i << "] = " << a[i] << "\n";

   sycl::free(a, q);
}
