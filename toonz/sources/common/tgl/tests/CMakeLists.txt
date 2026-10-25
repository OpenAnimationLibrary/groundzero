# Added from the top-level directory, so that the test is compiled as a
# client of tnzcore (without tnzcore's *_EXPORTS definitions).
add_executable(tglbatch_test tglbatch_test.cpp)
target_link_libraries(tglbatch_test PRIVATE tnzcore)
add_test(NAME tglbatch COMMAND tglbatch_test)
