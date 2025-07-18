# Generate the dictionaries (just like Makefile does with genreflex)
ROOTTEST_GENERATE_REFLEX_DICTIONARY(WrapRandomClasses
  WrapRandomClasses.h
  SELECTION WrapRandomClasses.xml
)

ROOTTEST_GENERATE_REFLEX_DICTIONARY(Dummy
  Dummy.h
  SELECTION Dummy.xml
  DEPENDS ${WrapRandomClasses}
)

# Add tests matching the *.script files and their expected *.ref output
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

ROOTTEST_ADD_TEST(execCMS
  MACRO scripts/case1.C
  OUTREF case1.ref
  DEPENDS ${WrapRandomClasses} ${Dummy}
)

ROOTTEST_ADD_TESTDIRS()
