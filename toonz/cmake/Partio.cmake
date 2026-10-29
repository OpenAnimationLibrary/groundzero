# SPDX-License-Identifier: BSD-3-Clause
# Complete pinned Partio core, including every upstream reader and writer.
# Build its target here to avoid the upstream application's global compiler,
# RPATH and installation settings leaking into OpenToonz.
include(FetchContent)
cmake_policy(PUSH)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_Declare(ot_partio
    URL https://github.com/wdas/partio/archive/cb91fd734fce64d01a3ff8fc4f293e7142c1838a.tar.gz
    URL_HASH SHA256=b6782dccef30fe044cdefb40168a46ac4d8edf87cba24723548187595c8b2b38)
FetchContent_GetProperties(ot_partio)
if(NOT ot_partio_POPULATED)
    FetchContent_Populate(ot_partio)
endif()
find_package(Threads REQUIRED)
file(GLOB OT_PARTIO_SOURCES CONFIGURE_DEPENDS
    "${ot_partio_SOURCE_DIR}/src/lib/core/*.cpp"
    "${ot_partio_SOURCE_DIR}/src/lib/io/*.cpp")
include("${CMAKE_CURRENT_LIST_DIR}/PartioPRT.cmake")
set(OT_PARTIO_PRT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/partio-patched/PRT.cpp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/partio-patched")
ot_patch_partio_prt("${ot_partio_SOURCE_DIR}/src/lib/io/PRT.cpp" "${OT_PARTIO_PRT_SOURCE}")
list(REMOVE_ITEM OT_PARTIO_SOURCES "${ot_partio_SOURCE_DIR}/src/lib/io/PRT.cpp")
list(APPEND OT_PARTIO_SOURCES "${OT_PARTIO_PRT_SOURCE}")
add_library(ot_partio_core STATIC ${OT_PARTIO_SOURCES})
add_library(Partio::Partio ALIAS ot_partio_core)
set_target_properties(ot_partio_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(ot_partio_core PUBLIC cxx_std_17)
target_include_directories(ot_partio_core PUBLIC "${ot_partio_SOURCE_DIR}/src/lib")
target_include_directories(ot_partio_core PRIVATE "${ot_partio_SOURCE_DIR}/src/lib/io")
target_compile_definitions(ot_partio_core PRIVATE PARTIO_USE_ZLIB)
target_link_libraries(ot_partio_core PUBLIC Threads::Threads)
if(BUILD_ENV_MSVC)
    # The Windows SDK already supplies the verified zlib used by image I/O.
    target_compile_definitions(ot_partio_core PRIVATE PARTIO_WIN32 _USE_MATH_DEFINES)
    # Upstream's PRT implementation is portable C++/zlib, but this historical
    # guard selects no-op Windows stubs. Undefine only its private switch for
    # this translation unit; preserve _WIN32 and every other platform macro.
    set_source_files_properties("${OT_PARTIO_PRT_SOURCE}"
        PROPERTIES COMPILE_OPTIONS /UPARTIO_WIN32)
    target_include_directories(ot_partio_core PRIVATE "${SDKROOT}/zlib-1.2.8")
    target_link_libraries(ot_partio_core PUBLIC ${Z_LIB})
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(ot_partio_core PUBLIC ZLIB::ZLIB)
endif()
install(FILES "${ot_partio_SOURCE_DIR}/LICENSE"
    DESTINATION share/opentoonz/licenses RENAME Partio-LICENSE.txt)
# Windows/macOS package directly from the build tree rather than cmake install.
configure_file("${ot_partio_SOURCE_DIR}/LICENSE"
    "${CMAKE_BINARY_DIR}/licenses/Partio-LICENSE.txt" COPYONLY)
cmake_policy(POP)
