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
include("${CMAKE_CURRENT_LIST_DIR}/PartioGEO.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PartioPTC.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PartioBIN.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PartioBGEO.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PartioMC.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PartioPDC.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PartioPDA.cmake")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/partio-patched")
foreach(_format PRT GEO PTC BIN BGEO MC PDC PDA)
    set(_patched "${CMAKE_CURRENT_BINARY_DIR}/partio-patched/${_format}.cpp")
    if(_format STREQUAL "PRT")
        set(OT_PARTIO_PRT_SOURCE "${_patched}")
        ot_patch_partio_prt("${ot_partio_SOURCE_DIR}/src/lib/io/PRT.cpp" "${_patched}")
    elseif(_format STREQUAL "GEO")
        ot_patch_partio_geo("${ot_partio_SOURCE_DIR}/src/lib/io/GEO.cpp" "${_patched}")
    elseif(_format STREQUAL "PTC")
        ot_patch_partio_ptc("${ot_partio_SOURCE_DIR}/src/lib/io/PTC.cpp" "${_patched}")
    elseif(_format STREQUAL "BIN")
        ot_patch_partio_bin("${ot_partio_SOURCE_DIR}/src/lib/io/BIN.cpp" "${_patched}")
    elseif(_format STREQUAL "BGEO")
        ot_patch_partio_bgeo("${ot_partio_SOURCE_DIR}/src/lib/io/BGEO.cpp" "${_patched}")
    elseif(_format STREQUAL "MC")
        ot_patch_partio_mc("${ot_partio_SOURCE_DIR}/src/lib/io/MC.cpp" "${_patched}")
    elseif(_format STREQUAL "PDC")
        ot_patch_partio_pdc("${ot_partio_SOURCE_DIR}/src/lib/io/PDC.cpp" "${_patched}")
    else()
        ot_patch_partio_pda("${ot_partio_SOURCE_DIR}/src/lib/io/PDA.cpp" "${_patched}")
    endif()
    list(REMOVE_ITEM OT_PARTIO_SOURCES "${ot_partio_SOURCE_DIR}/src/lib/io/${_format}.cpp")
    list(APPEND OT_PARTIO_SOURCES "${_patched}")
endforeach()
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
