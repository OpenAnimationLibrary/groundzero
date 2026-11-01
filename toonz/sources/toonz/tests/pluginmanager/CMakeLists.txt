cmake_minimum_required(VERSION 3.16)
project(PluginManagerTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(DEFINED PLUGIN_MANAGER_QT_MAJOR)
    set(QT_VERSION_MAJOR ${PLUGIN_MANAGER_QT_MAJOR})
else()
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets)
endif()
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets)
enable_testing()
add_executable(pluginmanager_test
    ../pluginmanager_test.cpp
    ../../pluginmanagerpopup.cpp
    ../../pluginmanagerpopup.h
    ../../../toonzqt/pluginmanager.cpp
    ../../../include/toonzqt/pluginmanager.h)
target_compile_definitions(pluginmanager_test PRIVATE TOONZQT_EXPORTS)
target_include_directories(pluginmanager_test PRIVATE ../.. ../../../include)
target_link_libraries(pluginmanager_test PRIVATE Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
add_test(NAME pluginmanager COMMAND pluginmanager_test)
set_tests_properties(pluginmanager PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
