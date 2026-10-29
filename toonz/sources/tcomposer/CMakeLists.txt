add_executable(tcomposer
    tcomposer.cpp
)

target_link_libraries(tcomposer
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    Qt${QT_VERSION_MAJOR}::Widgets
    toonzlib
    tfarm
    tnzstdfx
    sound
    image
    colorfx
    toonzqt
)

if(BUILD_SEEXPR_TESTS)
    add_executable(seexpr_batch_fixture tests/seexpr_batch_fixture.cpp)
    target_link_libraries(seexpr_batch_fixture PRIVATE
        toonzlib tnzbase tnzcore tnzstdfx
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
    add_dependencies(seexpr_batch_fixture tcomposer)
    add_test(NAME seexpr_native_batch
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/../../ci-scripts/seexpr/native_batch_test.py"
            --builder "$<TARGET_FILE:seexpr_batch_fixture>"
            --composer "$<TARGET_FILE:tcomposer>"
            --stuff "${CMAKE_SOURCE_DIR}/../../stuff"
            --report "${CMAKE_BINARY_DIR}/seexpr-native-batch-output.txt")
    set_tests_properties(seexpr_native_batch PROPERTIES TIMEOUT 720)
endif()
