include_directories(${CMAKE_CURRENT_SOURCE_DIR}/inc)

ROOTTEST_GENERATE_REFLEX_DICTIONARY(TemplatePackNormalization
                                    inc/VariadicConflictTrigger.h
                                    SELECTION inc/variadic_selection.xml
                                    FIXTURES_SETUP root-meta-templatePackNormalization-fixture)
