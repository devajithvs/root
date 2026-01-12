include_directories(${CMAKE_CURRENT_SOURCE_DIR})

ROOTTEST_GENERATE_DICTIONARY(SecondStaticAssert secondStaticAssert.h LINKDEF LinkDef.h
                             FIXTURES_SETUP root-meta-templateUnload-secondStaticAssert-fixture)

ROOTTEST_ADD_TEST(SecondStaticAssertTest
                  MACRO secondStaticAssertTest.C
                  OUTCNVCMD grep "static assertion failed"
                  OUTREF secondStaticAssertTest.ref
                  FIXTURES_REQUIRED  root-meta-templateUnload-secondStaticAssert-fixture
)
