# Built from tnztools/CMakeLists.txt with -DBUILD_TNZTOOLS_TESTS=ON, or
# configured on its own from this directory for a quick check.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(TnzToolsTests LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    find_package(Qt5 5.14 REQUIRED COMPONENTS Core Gui)
    if(UNIX AND NOT APPLE)
        add_definitions(-DLINUX)
    endif()
    enable_testing()
endif()

add_executable(typetooltext_test typetooltext_test.cpp ../typetooltext.cpp)
target_include_directories(typetooltext_test PRIVATE ..)
target_link_libraries(typetooltext_test PRIVATE Qt5::Core Qt5::Gui)
add_test(NAME typetooltext COMMAND typetooltext_test)
set_tests_properties(typetooltext PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(vectoralignment_test vectoralignment_test.cpp)
target_include_directories(vectoralignment_test PRIVATE ../../include)
add_test(NAME vectoralignment COMMAND vectoralignment_test)
