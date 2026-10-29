cmake_minimum_required(VERSION 3.20)
project(SeExprInstalledSDKConsumer LANGUAGES C CXX)
find_package(GroundzeroSeExpr 2.0.0 EXACT REQUIRED CONFIG)
add_executable(seexpr_installed_sdk
  "${CMAKE_CURRENT_LIST_DIR}/../../../toonz/sources/seexprsdk/host_callback_example.cpp")
target_compile_features(seexpr_installed_sdk PRIVATE cxx_std_17)
target_link_libraries(seexpr_installed_sdk PRIVATE GroundzeroSeExpr::Core)
enable_testing()
add_test(NAME seexpr_installed_sdk COMMAND seexpr_installed_sdk)
