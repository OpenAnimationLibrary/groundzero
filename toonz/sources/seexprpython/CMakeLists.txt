# Python inspection is a separate developer module, never an application
# runtime dependency. Build it against the interpreter selected for this SDK.
find_package(Python3 REQUIRED COMPONENTS Interpreter Development.Module)
if(NOT DEFINED GROUNDZERO_SEEXPR_SOURCE_DIR)
  message(FATAL_ERROR "SeExpr source provenance must be configured first")
endif()
set(_inspection_root "${CMAKE_CURRENT_BINARY_DIR}/inspection-parser")
set(_inspection_parser "${_inspection_root}/SeExpr2/parser")
set(_inspection_inputs)
set(_inspection_outputs)
foreach(_name ASTNode.h SeExprLex.h SeExprLex.cpp SeExprParse.h)
  list(APPEND _inspection_inputs "${GROUNDZERO_SEEXPR_SOURCE_DIR}/src/SeExpr2/parser/${_name}")
  list(APPEND _inspection_outputs "${_inspection_parser}/${_name}")
endforeach()
add_custom_command(
  OUTPUT "${_inspection_parser}/prepared.stamp"
  BYPRODUCTS ${_inspection_outputs}
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_parser.py"
    --source "${GROUNDZERO_SEEXPR_SOURCE_DIR}" --output "${_inspection_root}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/prepare_parser.py" ${_inspection_inputs}
  COMMENT "Preparing the pinned inspection parser ownership and source ranges"
  VERBATIM)
add_custom_target(seexpr_python_parser
  DEPENDS "${_inspection_parser}/prepared.stamp")
set_source_files_properties("${_inspection_parser}/SeExprLex.cpp" PROPERTIES GENERATED TRUE)
Python3_add_library(seexpr_python_core MODULE WITH_SOABI
  astmodule.cpp
  evalmodule.cpp
  "${_inspection_parser}/SeExprLex.cpp")
add_dependencies(seexpr_python_core seexpr_python_parser)
target_compile_features(seexpr_python_core PRIVATE cxx_std_17)
target_include_directories(seexpr_python_core SYSTEM PRIVATE
  "${_inspection_root}")
if(TARGET seexpr_native_adapter)
  set_target_properties(seexpr_native_adapter PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(seexpr_python_core PRIVATE seexpr_native_adapter)
else()
  add_library(seexpr_python_adapter STATIC ../common/expressions/tseexpr.cpp)
  set_target_properties(seexpr_python_adapter PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_features(seexpr_python_adapter PRIVATE cxx_std_17)
  target_include_directories(seexpr_python_adapter PUBLIC ../include)
  target_compile_definitions(seexpr_python_adapter PUBLIC TNZBASE_EXPORTS)
  target_link_libraries(seexpr_python_adapter PUBLIC groundzero_seexpr_core)
  target_link_libraries(seexpr_python_core PRIVATE seexpr_python_adapter)
endif()
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
configure_file(SeExprPy/editor.py "${_python_package}/editor.py" COPYONLY)
configure_file(expreditor2.py "${CMAKE_BINARY_DIR}/seexpr-python/expreditor2.py" COPYONLY)
install(TARGETS seexpr_python_core LIBRARY DESTINATION sdk/python/SeExprPy
  RUNTIME DESTINATION sdk/python/SeExprPy)
install(FILES SeExprPy/__init__.py SeExprPy/editor.py DESTINATION sdk/python/SeExprPy)
install(FILES expreditor2.py DESTINATION sdk/python)
option(BUILD_SEEXPR_PYTHON_UI_TESTS "Qualify the optional Python Qt editor SDK" OFF)
if(BUILD_SEEXPR_TESTS)
  # Executing the source test directly prepends its directory to sys.path and
  # imports the unbuilt source package. Put the qualified package first before
  # run_path sets up the test so both bindings and Python files share one SDK.
  set(_sdk_runner "import runpy,sys;sys.path.insert(0,sys.argv[1]);runpy.run_path(sys.argv[2],run_name='__main__')")
  foreach(_case ast eval)
    add_test(NAME seexpr_python_${_case}
      COMMAND "${Python3_EXECUTABLE}" -c "${_sdk_runner}"
      "${CMAKE_BINARY_DIR}/seexpr-python" "${CMAKE_CURRENT_SOURCE_DIR}/${_case}_test.py")
    set_tests_properties(seexpr_python_${_case} PROPERTIES TIMEOUT 120)
  endforeach()
  if(BUILD_SEEXPR_PYTHON_UI_TESTS)
    add_test(NAME seexpr_python_ui
      COMMAND "${CMAKE_COMMAND}" -E env
      "QT_QPA_PLATFORM=offscreen" "SEEXPR_QT_API=pyqt5"
      "${Python3_EXECUTABLE}" -c "${_sdk_runner}"
      "${CMAKE_BINARY_DIR}/seexpr-python" "${CMAKE_CURRENT_SOURCE_DIR}/ui_test.py")
    set_tests_properties(seexpr_python_ui PROPERTIES TIMEOUT 180)
  endif()
endif()
