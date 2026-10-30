cmake_minimum_required(VERSION 3.16)
project(BrushParserRegression LANGUAGES CXX)
enable_testing()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
add_executable(abrparser_test
  "${REPO_ROOT}/toonz/sources/toonzlib/abrparser.cpp"
  "${REPO_ROOT}/toonz/sources/toonzlib/tests/abrparser_test.cpp")
target_include_directories(abrparser_test PRIVATE "${REPO_ROOT}/toonz/sources/include")
target_compile_definitions(abrparser_test PRIVATE TOONZLIB_EXPORTS)
if(APPLE)
  target_compile_definitions(abrparser_test PRIVATE MACOSX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(abrparser_test PRIVATE LINUX)
endif()

add_executable(rasterbrushbenchmark
  "${REPO_ROOT}/toonz/sources/toonzlib/rasterbrushengine.cpp"
  "${REPO_ROOT}/ci-scripts/brush-reference/benchmark.cpp")
target_include_directories(rasterbrushbenchmark PRIVATE "${REPO_ROOT}/toonz/sources/include")
target_compile_definitions(rasterbrushbenchmark PRIVATE TOONZLIB_EXPORTS)
if(APPLE)
  target_compile_definitions(rasterbrushbenchmark PRIVATE MACOSX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(rasterbrushbenchmark PRIVATE LINUX)
endif()
option(BRUSH_SANITIZERS "Run parser regression with address/undefined sanitizers" OFF)
if(BRUSH_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(abrparser_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(abrparser_test PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME abrparser COMMAND abrparser_test)
set_tests_properties(abrparser PROPERTIES TIMEOUT 60)

add_executable(rasterbrushengine_test
  "${REPO_ROOT}/toonz/sources/toonzlib/rasterbrushengine.cpp"
  "${REPO_ROOT}/toonz/sources/toonzlib/tests/rasterbrushengine_test.cpp")
target_include_directories(rasterbrushengine_test PRIVATE "${REPO_ROOT}/toonz/sources/include")
target_compile_definitions(rasterbrushengine_test PRIVATE TOONZLIB_EXPORTS)
if(APPLE)
  target_compile_definitions(rasterbrushengine_test PRIVATE MACOSX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(rasterbrushengine_test PRIVATE LINUX)
endif()
if(BRUSH_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(rasterbrushengine_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(rasterbrushengine_test PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rasterbrushengine COMMAND rasterbrushengine_test)
set_tests_properties(rasterbrushengine PROPERTIES TIMEOUT 60)

set(BRUSH_QT_MAJOR 6 CACHE STRING "Qt major for the package adapter tests")
find_package(Qt${BRUSH_QT_MAJOR} REQUIRED COMPONENTS Core Gui)
find_package(ZLIB REQUIRED)
add_executable(rasterbrushpreset_test
  "${REPO_ROOT}/toonz/sources/toonzlib/rasterbrushpreset.cpp"
  "${REPO_ROOT}/toonz/sources/toonzlib/rasterbrushimport.cpp"
  "${REPO_ROOT}/toonz/sources/toonzlib/rasterbrushpng.cpp"
  "${REPO_ROOT}/toonz/sources/toonzlib/rasterbrusharchive.cpp"
  "${REPO_ROOT}/toonz/sources/toonzlib/abrparser.cpp"
  "${REPO_ROOT}/toonz/sources/toonzlib/rasterbrushengine.cpp"
  "${REPO_ROOT}/toonz/sources/toonzlib/tests/rasterbrushpreset_test.cpp")
target_include_directories(rasterbrushpreset_test PRIVATE "${REPO_ROOT}/toonz/sources/include")
target_compile_definitions(rasterbrushpreset_test PRIVATE TOONZLIB_EXPORTS)
target_link_libraries(rasterbrushpreset_test PRIVATE Qt${BRUSH_QT_MAJOR}::Core Qt${BRUSH_QT_MAJOR}::Gui ZLIB::ZLIB)
if(APPLE)
  target_compile_definitions(rasterbrushpreset_test PRIVATE MACOSX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(rasterbrushpreset_test PRIVATE LINUX)
endif()
if(BRUSH_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(rasterbrushpreset_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(rasterbrushpreset_test PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME rasterbrushpreset COMMAND rasterbrushpreset_test)
set_tests_properties(rasterbrushpreset PROPERTIES TIMEOUT 60)

get_target_property(BRUSH_ADAPTER_SOURCES rasterbrushpreset_test SOURCES)
list(REMOVE_ITEM BRUSH_ADAPTER_SOURCES "${REPO_ROOT}/toonz/sources/toonzlib/tests/rasterbrushpreset_test.cpp")
add_executable(rasterbrushreference ${BRUSH_ADAPTER_SOURCES}
  "${REPO_ROOT}/ci-scripts/brush-reference/compare.cpp")
target_include_directories(rasterbrushreference PRIVATE "${REPO_ROOT}/toonz/sources/include")
target_compile_definitions(rasterbrushreference PRIVATE TOONZLIB_EXPORTS)
target_link_libraries(rasterbrushreference PRIVATE Qt${BRUSH_QT_MAJOR}::Core Qt${BRUSH_QT_MAJOR}::Gui ZLIB::ZLIB)
if(APPLE)
  target_compile_definitions(rasterbrushreference PRIVATE MACOSX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(rasterbrushreference PRIVATE LINUX)
endif()
