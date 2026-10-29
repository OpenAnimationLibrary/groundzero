# Standalone SDK and utilities use the same pinned core and parser as the
# application. No developer plugin is installed into an automatic scene path.
include(CMakePackageConfigHelpers)
option(OT_SEEXPR_BUILD_SDK "Build the SeExpr developer SDK and command-line utilities" ON)
if(NOT OT_SEEXPR_BUILD_SDK)
  return()
endif()
get_filename_component(_seexpr_native_source "${CMAKE_CURRENT_LIST_DIR}/../sources" ABSOLUTE)
get_filename_component(_seexpr_repository "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set_target_properties(groundzero_seexpr_core PROPERTIES EXPORT_NAME Core)
add_library(GroundzeroSeExpr::Core ALIAS groundzero_seexpr_core)
install(TARGETS groundzero_seexpr_core EXPORT GroundzeroSeExprTargets
  ARCHIVE DESTINATION sdk/lib)
install(FILES ${_ot_seexpr_header_outputs} DESTINATION sdk/include/SeExpr2)
if(OT_SEEXPR_WITH_LLVM)
  install(FILES
    "${CMAKE_CURRENT_LIST_DIR}/seexpr_llvm_compat.h"
    "${_seexpr_native_source}/common/expressions/seexprbudget.h"
    DESTINATION sdk/include/SeExpr2)
endif()
install(FILES "${_seexpr_repository}/stuff/doc/LICENSE/LICENSE_seexpr.txt"
  DESTINATION sdk/share/licenses)
configure_package_config_file(
  "${CMAKE_CURRENT_LIST_DIR}/SeExprSDKConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/GroundzeroSeExprConfig.cmake"
  INSTALL_DESTINATION sdk/lib/cmake/GroundzeroSeExpr)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/GroundzeroSeExprConfigVersion.cmake"
  VERSION 2.0.0 COMPATIBILITY SameMajorVersion)
install(EXPORT GroundzeroSeExprTargets NAMESPACE GroundzeroSeExpr::
  DESTINATION sdk/lib/cmake/GroundzeroSeExpr)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/GroundzeroSeExprConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/GroundzeroSeExprConfigVersion.cmake"
  DESTINATION sdk/lib/cmake/GroundzeroSeExpr)

add_library(seexpr_sdk_adapter STATIC
  "${_seexpr_native_source}/common/expressions/tseexpr.cpp")
target_compile_features(seexpr_sdk_adapter PRIVATE cxx_std_17)
target_compile_definitions(seexpr_sdk_adapter PRIVATE TNZBASE_EXPORTS)
if(APPLE)
  target_compile_definitions(seexpr_sdk_adapter PUBLIC MACOSX)
elseif(UNIX)
  target_compile_definitions(seexpr_sdk_adapter PUBLIC LINUX)
endif()
target_include_directories(seexpr_sdk_adapter PUBLIC "${_seexpr_native_source}/include")
target_link_libraries(seexpr_sdk_adapter PRIVATE groundzero_seexpr_core)
if(MSVC)
  target_compile_definitions(seexpr_sdk_adapter PRIVATE _USE_MATH_DEFINES)
endif()
foreach(_utility calc graph)
  add_executable(seexpr-${_utility} "${_seexpr_native_source}/seexprsdk/seexpr_cli.cpp")
  target_compile_features(seexpr-${_utility} PRIVATE cxx_std_17)
  target_compile_definitions(seexpr-${_utility} PRIVATE TNZBASE_EXPORTS)
  target_link_libraries(seexpr-${_utility} PRIVATE seexpr_sdk_adapter groundzero_seexpr_core)
  install(TARGETS seexpr-${_utility} RUNTIME DESTINATION sdk/bin)
endforeach()
add_executable(seexpr_host_example "${_seexpr_native_source}/seexprsdk/host_callback_example.cpp")
target_compile_features(seexpr_host_example PRIVATE cxx_std_17)
target_link_libraries(seexpr_host_example PRIVATE groundzero_seexpr_core)
install(FILES "${_seexpr_native_source}/seexprsdk/host_callback_example.cpp"
  DESTINATION sdk/share/examples)
install(FILES "${_seexpr_repository}/ci-scripts/seexpr/plugin_fixture.cpp"
  DESTINATION sdk/share/examples)

if(BUILD_TESTING OR BUILD_SEEXPR_TESTS)
  add_library(seexpr_plugin_fixture MODULE "${_seexpr_repository}/ci-scripts/seexpr/plugin_fixture.cpp")
  target_link_libraries(seexpr_plugin_fixture PRIVATE groundzero_seexpr_core)
  target_compile_features(seexpr_plugin_fixture PRIVATE cxx_std_17)
  set_target_properties(seexpr_plugin_fixture PROPERTIES
    PREFIX "" OUTPUT_NAME SeExprSdkFixture)
  add_library(seexpr_invalid_plugin_fixture MODULE
    "${_seexpr_repository}/ci-scripts/seexpr/plugin_invalid_fixture.cpp")
  set_target_properties(seexpr_invalid_plugin_fixture PROPERTIES
    PREFIX "" OUTPUT_NAME SeExprInvalidFixture)
  if(WIN32)
    # Windows packaging copies the application's runtime directory. Keep test
    # plugins and import/debug artifacts outside it in every configuration.
    set(_seexpr_plugin_fixture_output "${CMAKE_BINARY_DIR}/seexpr-test-plugins/$<CONFIG>")
    set_target_properties(seexpr_plugin_fixture seexpr_invalid_plugin_fixture PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${_seexpr_plugin_fixture_output}"
      LIBRARY_OUTPUT_DIRECTORY "${_seexpr_plugin_fixture_output}"
      ARCHIVE_OUTPUT_DIRECTORY "${_seexpr_plugin_fixture_output}"
      PDB_OUTPUT_DIRECTORY "${_seexpr_plugin_fixture_output}")
  endif()
  add_executable(seexpr_plugin_test "${_seexpr_repository}/ci-scripts/seexpr/sdk_plugin_test.cpp")
  target_compile_features(seexpr_plugin_test PRIVATE cxx_std_17)
  target_link_libraries(seexpr_plugin_test PRIVATE groundzero_seexpr_core)
  add_dependencies(seexpr_plugin_test seexpr_plugin_fixture seexpr_invalid_plugin_fixture)
  add_test(NAME seexpr_sdk_plugins COMMAND "${CMAKE_COMMAND}" -E env
    "SE_EXPR_PLUGINS=$<TARGET_FILE:seexpr_plugin_fixture>"
    "$<TARGET_FILE:seexpr_plugin_test>"
    "$<TARGET_FILE:seexpr_plugin_fixture>" "$<TARGET_FILE:seexpr_invalid_plugin_fixture>")
  set_tests_properties(seexpr_sdk_plugins PROPERTIES TIMEOUT 60)
  add_test(NAME seexpr_sdk_host_callbacks COMMAND seexpr_host_example)
  add_test(NAME seexpr_sdk_calculator COMMAND seexpr-calc "2+3*4")
  set_tests_properties(seexpr_sdk_calculator PROPERTIES PASS_REGULAR_EXPRESSION "14")
  add_test(NAME seexpr_sdk_grapher COMMAND seexpr-graph "sin(x)" "-3.14" "3.14")
  set_tests_properties(seexpr_sdk_grapher PROPERTIES PASS_REGULAR_EXPRESSION "#")
endif()
