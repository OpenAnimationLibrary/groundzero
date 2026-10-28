# Built from tnztools/CMakeLists.txt with -DBUILD_TNZTOOLS_TESTS=ON, or
# configured on its own from this directory for a quick check.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(TnzToolsTests LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(QT_VERSION_MAJOR 5)
    find_package(Qt5 5.14 REQUIRED COMPONENTS Core Gui)
    if(UNIX AND NOT APPLE)
        add_definitions(-DLINUX)
    endif()
    enable_testing()
endif()

set(TEST_QT_LIBS Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
if(QT_VERSION_MAJOR EQUAL 6)
    # QTextCodec
    list(APPEND TEST_QT_LIBS Qt6::Core5Compat)
endif()

add_executable(typetooltext_test typetooltext_test.cpp ../typetooltext.cpp)
target_include_directories(typetooltext_test PRIVATE ..)
target_link_libraries(typetooltext_test PRIVATE ${TEST_QT_LIBS})
add_test(NAME typetooltext COMMAND typetooltext_test)
set_tests_properties(typetooltext PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(vectoralignment_test vectoralignment_test.cpp)
target_include_directories(vectoralignment_test PRIVATE ../../include)
add_test(NAME vectoralignment COMMAND vectoralignment_test)

add_executable(widthprofile_test widthprofile_test.cpp)
target_include_directories(widthprofile_test PRIVATE ../../include)
add_test(NAME widthprofile COMMAND widthprofile_test)

add_executable(tinbetweenmatch_test tinbetweenmatch_test.cpp)
target_include_directories(tinbetweenmatch_test PRIVATE ../../include)
add_test(NAME tinbetweenmatch COMMAND tinbetweenmatch_test)

add_executable(bilineardeform_test bilineardeform_test.cpp)
target_include_directories(bilineardeform_test PRIVATE ../../include)
add_test(NAME bilineardeform COMMAND bilineardeform_test)

add_executable(fluidwidth_test fluidwidth_test.cpp)
target_include_directories(fluidwidth_test PRIVATE ../../include)
add_test(NAME fluidwidth COMMAND fluidwidth_test)

# artlayer.h needs TStroke, so only in the full build
if(TARGET tnzcore)
    add_executable(artlayer_test artlayer_test.cpp)
    target_include_directories(artlayer_test PRIVATE ../../include)
    target_link_libraries(artlayer_test PRIVATE tnzcore ${TEST_QT_LIBS})
    add_test(NAME artlayer COMMAND artlayer_test)
endif()
