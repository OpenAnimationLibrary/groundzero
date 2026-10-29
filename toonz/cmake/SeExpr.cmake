# SPDX-License-Identifier: BSD-3-Clause
# Build the pinned expression core without importing upstream's project-wide
# compiler flags, Qt editor, Python installation or dependency options.

include(FetchContent)
cmake_policy(PUSH)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(OT_SEEXPR_REVISION "8f8c8f2c5e27e96fae70d6b82ac1ff4f4811d6dc")
set(OT_SEEXPR_SOURCE_SHA256
    "097ffec1a4f50db2b8b7d8f8d0e488e0cafb9f235f1a63bc3693e6d6f079a8bb")

FetchContent_Declare(
    groundzero_seexpr_source
    URL "https://codeload.github.com/wdas/SeExpr/tar.gz/${OT_SEEXPR_REVISION}"
    URL_HASH "SHA256=${OT_SEEXPR_SOURCE_SHA256}"
)
FetchContent_GetProperties(groundzero_seexpr_source)
if(NOT groundzero_seexpr_source_POPULATED)
    # Only populate the archive. Its top-level CMakeLists is intentionally not
    # evaluated: all library settings below are private to this dependency.
    FetchContent_Populate(groundzero_seexpr_source)
endif()
set(GROUNDZERO_SEEXPR_SOURCE_DIR "${groundzero_seexpr_source_SOURCE_DIR}")

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_program(OT_SEEXPR_BISON_EXECUTABLE NAMES bison win_bison)
find_program(OT_SEEXPR_FLEX_EXECUTABLE NAMES flex win_flex)
if(NOT OT_SEEXPR_BISON_EXECUTABLE OR NOT OT_SEEXPR_FLEX_EXECUTABLE)
    message(FATAL_ERROR "SeExpr requires Bison 3.8.2 and Flex 2.6.4 (bison/win_bison and flex/win_flex)")
endif()
find_package(Threads REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/SeExprLLVM.cmake")

set(_ot_seexpr_core_names
    Context.cpp
    ContextUtils.cpp
    Curve.cpp
    ExprBuiltins.cpp
    ExprEnv.cpp
    ExprFunc.cpp
    ExprFuncStandard.cpp
    ExprFuncX.cpp
    ExprLLVMCodeGeneration.cpp
    ExprMultiExpr.cpp
    ExprNode.cpp
    ExprWalker.cpp
    Expression.cpp
    Interpreter.cpp
    Noise.cpp
    Platform.cpp
    SeContext.cpp
)
set(_ot_seexpr_generated_root "${CMAKE_CURRENT_BINARY_DIR}/seexpr-generated")
set(_ot_seexpr_generated "${_ot_seexpr_generated_root}/SeExpr2")
set(_ot_seexpr_sources)
foreach(_name IN LISTS _ot_seexpr_core_names)
    list(APPEND _ot_seexpr_sources "${_ot_seexpr_generated}/${_name}")
endforeach()
list(APPEND _ot_seexpr_sources
    "${_ot_seexpr_generated}/ExprParser.cpp"
    "${_ot_seexpr_generated}/ExprParserLex.cpp")

file(GLOB _ot_seexpr_input_headers
    "${groundzero_seexpr_source_SOURCE_DIR}/src/SeExpr2/*.h")
set(_ot_seexpr_header_outputs)
foreach(_header IN LISTS _ot_seexpr_input_headers)
    get_filename_component(_name "${_header}" NAME)
    list(APPEND _ot_seexpr_header_outputs "${_ot_seexpr_generated}/${_name}")
endforeach()
list(APPEND _ot_seexpr_header_outputs
    "${_ot_seexpr_generated}/ExprConfig.h"
    "${_ot_seexpr_generated}/ExprParser.tab.h")

set(_ot_seexpr_prepare "${CMAKE_CURRENT_LIST_DIR}/prepare_seexpr.py")
add_custom_command(
    OUTPUT "${_ot_seexpr_generated}/prepared.stamp"
    BYPRODUCTS ${_ot_seexpr_sources} ${_ot_seexpr_header_outputs}
    COMMAND "${Python3_EXECUTABLE}" "${_ot_seexpr_prepare}"
        --source "${groundzero_seexpr_source_SOURCE_DIR}"
        --output "${_ot_seexpr_generated}"
        --bison "${OT_SEEXPR_BISON_EXECUTABLE}"
        --flex "${OT_SEEXPR_FLEX_EXECUTABLE}"
        ${_ot_seexpr_llvm_prepare_args}
    DEPENDS
        "${_ot_seexpr_prepare}"
        "${CMAKE_CURRENT_LIST_DIR}/prepare_seexpr_language.py"
        "${CMAKE_CURRENT_LIST_DIR}/prepare_seexpr_safety.py"
        "${CMAKE_CURRENT_LIST_DIR}/prepare_seexpr_sdk.py"
        "${CMAKE_CURRENT_LIST_DIR}/prepare_seexpr_limits.py"
        "${CMAKE_CURRENT_LIST_DIR}/prepare_seexpr_llvm.py"
        "${CMAKE_CURRENT_LIST_DIR}/seexpr_llvm_compat.h"
        "${groundzero_seexpr_source_SOURCE_DIR}/src/SeExpr2/ExprParser.y"
        "${groundzero_seexpr_source_SOURCE_DIR}/src/SeExpr2/ExprParserLex.l"
        ${_ot_seexpr_input_headers}
    COMMENT "Preparing the pinned SeExpr core and common parser"
    VERBATIM
)
add_custom_target(groundzero_seexpr_generated
    DEPENDS "${_ot_seexpr_generated}/prepared.stamp")
set_source_files_properties(${_ot_seexpr_sources} PROPERTIES GENERATED TRUE)
add_library(groundzero_seexpr_core STATIC ${_ot_seexpr_sources}
    "${CMAKE_CURRENT_LIST_DIR}/../sources/common/expressions/seexprbudget.cpp")
add_dependencies(groundzero_seexpr_core groundzero_seexpr_generated)
set_target_properties(groundzero_seexpr_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)
target_include_directories(groundzero_seexpr_core SYSTEM PUBLIC
    "$<BUILD_INTERFACE:${_ot_seexpr_generated_root}>"
    "$<BUILD_INTERFACE:${_ot_seexpr_generated}>"
    "$<INSTALL_INTERFACE:sdk/include>"
    "$<INSTALL_INTERFACE:sdk/include/SeExpr2>")
target_include_directories(groundzero_seexpr_core PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../sources/common/expressions")
target_link_libraries(groundzero_seexpr_core PRIVATE
    Threads::Threads ${CMAKE_DL_LIBS})
if(OT_SEEXPR_WITH_LLVM)
    target_sources(groundzero_seexpr_core PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../sources/common/expressions/seexprllvmhelpers.cpp")
endif()
groundzero_seexpr_configure_llvm(groundzero_seexpr_core)
target_compile_definitions(groundzero_seexpr_core PRIVATE __STDC_LIMIT_MACROS)
if(MSVC)
    target_compile_definitions(groundzero_seexpr_core PUBLIC SEEXPR_WIN32)
    target_compile_definitions(groundzero_seexpr_core PRIVATE
        _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE NOMINMAX)
endif()

# Source provenance is independent of the selected, explicitly reported backend.
target_compile_definitions(groundzero_seexpr_core PUBLIC
    OT_SEEXPR_SOURCE_REVISION="${OT_SEEXPR_REVISION}")

include("${CMAKE_CURRENT_LIST_DIR}/SeExprSDK.cmake")

cmake_policy(POP)
