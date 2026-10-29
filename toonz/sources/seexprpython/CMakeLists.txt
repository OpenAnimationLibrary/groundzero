# Python inspection is a separate developer module, never an application
# runtime dependency. Build it against the interpreter selected for this SDK.
find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
if(NOT DEFINED GROUNDZERO_SEEXPR_SOURCE_DIR)
  message(FATAL_ERROR "SeExpr source provenance must be configured first")
endif()
Python3_add_library(seexpr_python_core MODULE WITH_SOABI
  astmodule.cpp
  "${GROUNDZERO_SEEXPR_SOURCE_DIR}/src/SeExpr2/parser/SeExprLex.cpp")
target_compile_features(seexpr_python_core PRIVATE cxx_std_17)
target_include_directories(seexpr_python_core SYSTEM PRIVATE
  "${GROUNDZERO_SEEXPR_SOURCE_DIR}/src")
set(_python_package "${CMAKE_BINARY_DIR}/seexpr-python/SeExprPy")
set_target_properties(seexpr_python_core PROPERTIES
  OUTPUT_NAME _core
  LIBRARY_OUTPUT_DIRECTORY "${_python_package}"
  RUNTIME_OUTPUT_DIRECTORY "${_python_package}")
foreach(_config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(seexpr_python_core PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${_config} "${_python_package}"
    RUNTIME_OUTPUT_DIRECTORY_${_config} "${_python_package}")
endforeach()
file(MAKE_DIRECTORY "${_python_package}")
configure_file(SeExprPy/__init__.py "${_python_package}/__init__.py" COPYONLY)
install(TARGETS seexpr_python_core LIBRARY DESTINATION sdk/python/SeExprPy
  RUNTIME DESTINATION sdk/python/SeExprPy)
install(FILES SeExprPy/__init__.py DESTINATION sdk/python/SeExprPy)
if(BUILD_SEEXPR_TESTS)
  add_test(NAME seexpr_python_ast
    COMMAND "${CMAKE_COMMAND}" -E env
    "PYTHONPATH=${CMAKE_BINARY_DIR}/seexpr-python"
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ast_test.py")
endif()
