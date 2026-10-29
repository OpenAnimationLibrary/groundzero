cmake_minimum_required(VERSION 3.20)
project(SeExprQualification LANGUAGES C CXX)

include(CTest)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(OT_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(OT_NATIVE_SOURCE "${OT_SOURCE_ROOT}/toonz/sources")

# This deliberately configures no Qt, graphics context or application library.
# Core compiler/parser errors must surface before the full shipping builds.
include("${OT_SOURCE_ROOT}/toonz/cmake/SeExpr.cmake")

add_library(seexpr_native_adapter STATIC
  "${OT_NATIVE_SOURCE}/common/expressions/tseexpr.cpp")
target_include_directories(seexpr_native_adapter PUBLIC
  "${OT_NATIVE_SOURCE}/include")
target_compile_definitions(seexpr_native_adapter PUBLIC TNZBASE_EXPORTS)
if(APPLE)
  target_compile_definitions(seexpr_native_adapter PUBLIC MACOSX)
elseif(UNIX)
  target_compile_definitions(seexpr_native_adapter PUBLIC LINUX)
endif()
if(MSVC)
  target_compile_definitions(seexpr_native_adapter PRIVATE _USE_MATH_DEFINES)
endif()
target_link_libraries(seexpr_native_adapter PUBLIC groundzero_seexpr_core)

add_executable(seexpr_runtime
  "${OT_NATIVE_SOURCE}/common/expressions/tests/tseexpr_test.cpp")
target_link_libraries(seexpr_runtime PRIVATE seexpr_native_adapter)
add_test(NAME seexpr_runtime COMMAND seexpr_runtime)
set_tests_properties(seexpr_runtime PROPERTIES TIMEOUT 120)

add_executable(seexpr_builtins builtin_test.cpp)
target_link_libraries(seexpr_builtins PRIVATE seexpr_native_adapter)
add_test(NAME seexpr_builtins COMMAND seexpr_builtins)
set_tests_properties(seexpr_builtins PROPERTIES TIMEOUT 180)

add_executable(seexpr_formatting builtin_test.cpp)
target_compile_definitions(seexpr_formatting PRIVATE SEEXPR_FORMATTING_ONLY)
target_link_libraries(seexpr_formatting PRIVATE seexpr_native_adapter)
add_test(NAME seexpr_formatting COMMAND seexpr_formatting)
set_tests_properties(seexpr_formatting PROPERTIES TIMEOUT 180)

add_executable(seexpr_sdk sdk_test.cpp)
target_link_libraries(seexpr_sdk PRIVATE groundzero_seexpr_core)
add_test(NAME seexpr_sdk COMMAND seexpr_sdk)
set_tests_properties(seexpr_sdk PROPERTIES TIMEOUT 60)

option(BUILD_SEEXPR_PYTHON "Qualify the optional Python AST SDK module" ON)
set(BUILD_SEEXPR_TESTS ON)
if(BUILD_SEEXPR_PYTHON)
  add_subdirectory("${OT_NATIVE_SOURCE}/seexprpython" seexprpython)
endif()

if(OT_SEEXPR_WITH_LLVM)
  add_executable(seexpr_llvm_test
    "${OT_NATIVE_SOURCE}/common/expressions/tests/seexpr_llvm_test.cpp")
  target_link_libraries(seexpr_llvm_test PRIVATE seexpr_native_adapter)
  add_test(NAME seexpr_llvm COMMAND seexpr_llvm_test)
  set_tests_properties(seexpr_llvm PROPERTIES TIMEOUT 120)
endif()
