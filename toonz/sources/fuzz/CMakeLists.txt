# Robustness tests for the file loaders. Each harness writes a few small
# valid files in code, then loads them and a fixed, deterministic set of
# corruptions of them (truncations, bit flips, length field changes). An
# exception from the loader is fine; a crash or a hang fails the test. Every
# case runs in a child process, so one crash does not hide the others. See
# fuzzdriver.h for the command line; to reproduce a failing case:
#
#   pli_fuzz_test --case '<case id>'            run it alone, e.g. in gdb
#   pli_fuzz_test --dump '<case id>' bad.pli    write its input to a file
#
# BUILD_FUZZ_TESTS builds <name>_fuzz_test and registers it with ctest.
# WITH_LIBFUZZER (Clang only) also builds <name>_fuzzer libFuzzer binaries,
# which are run by hand and not registered with ctest:
#
#   pli_fuzz_test --write-seeds corpus
#   pli_fuzzer -max_total_time=600 corpus
#
# A fuzzer instruments only the sources built into it (the PLI reader is);
# configure with CMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link,address to cover
# the libraries as well.

set(FUZZ_TEST_TIMEOUT 900)

# add_fuzz_harness(<name> [SOURCES <file>...] [LIBS <library>...])
# builds <name>_fuzz.cpp with the shared driver.
function(add_fuzz_harness NAME)
    cmake_parse_arguments(FUZZ "" "" "SOURCES;LIBS" ${ARGN})
    set(FUZZ_ALL_SOURCES fuzzdriver.h fuzzdriver.cpp ${NAME}_fuzz.cpp ${FUZZ_SOURCES})

    if(BUILD_FUZZ_TESTS)
        add_executable(${NAME}_fuzz_test ${FUZZ_ALL_SOURCES})
        target_link_libraries(${NAME}_fuzz_test PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${FUZZ_LIBS})
        add_test(NAME fuzz_${NAME} COMMAND ${NAME}_fuzz_test --timeout 20)
        set_tests_properties(fuzz_${NAME} PROPERTIES TIMEOUT ${FUZZ_TEST_TIMEOUT})
    endif()

    if(WITH_LIBFUZZER)
        add_executable(${NAME}_fuzzer ${FUZZ_ALL_SOURCES})
        target_compile_definitions(${NAME}_fuzzer PRIVATE OT_LIBFUZZER)
        target_compile_options(${NAME}_fuzzer PRIVATE -fsanitize=fuzzer,address)
        target_link_options(${NAME}_fuzzer PRIVATE -fsanitize=fuzzer,address)
        target_link_libraries(${NAME}_fuzzer PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${FUZZ_LIBS})
    endif()
endfunction()

# Scenes, palettes and fx parameters (common/tstream); the seeds include
# LZ4 compressed scenes.
add_fuzz_harness(tstream LIBS tnzcore tnzbase ${LZ4_LIB})

# PLI vector levels. The reader classes are not exported from the image
# library, so their sources are built into the harness.
add_fuzz_harness(pli
    SOURCES
        ../image/pli/pli_io.cpp
        ../image/pli/tags.cpp
        ../image/pli/tiio_pli.cpp
    LIBS tnzcore
)
foreach(_target pli_fuzz_test pli_fuzzer)
    if(TARGET ${_target})
        target_include_directories(${_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../image/pli)
    endif()
endforeach()

# PSD images (common/psdlib, exported from tnzcore).
add_fuzz_harness(psd LIBS tnzcore)
