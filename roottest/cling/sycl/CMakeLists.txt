if(experimental_adaptivecpp)
  ROOTTEST_ADD_TEST(syclbasic
                    MACRO syclbasic.C
                    OUTREF syclbasic.ref
                    LABELS roottest regression cling sycl)
endif()

