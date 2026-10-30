set(PREVIEW_SOURCES
    toonzpreview.cpp
    thumbnailprocess.cpp
    toonzpreview.def
)

set(PREVIEW_HEADERS
    toonzpreview.h
    thumbnailprocess.h
)

add_library(toonzpreview SHARED ${PREVIEW_SOURCES} ${PREVIEW_HEADERS})

target_compile_definitions(toonzpreview PRIVATE
    TOONZPREVIEW_EXPORTS
    _ATL_NO_AUTOMATIC_NAMESPACE
    UNICODE
    _UNICODE
)

message("subdir: toonz")
message("Bin: " ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(toonzpreview PRIVATE
    shlwapi
    shell32
    ole32
    uuid
    gdi32
    advapi32
    user32
)
add_dependencies(toonzpreview tconverter)

set_target_properties(toonzpreview PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
    OUTPUT_NAME "toonzpreview"
)

if(BUILD_UI_PORT_TESTS)
    add_executable(ui_shellthumbnail_test tests/shellthumbnail_test.cpp thumbnailprocess.cpp)
    target_compile_definitions(ui_shellthumbnail_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(ui_shellthumbnail_test PRIVATE
        toonzlib tnzcore image Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui
        ole32 uuid gdi32)
    add_dependencies(ui_shellthumbnail_test toonzpreview tconverter)
    add_dependencies(ui_port_tests ui_shellthumbnail_test)
    add_test(NAME ui_shellthumbnail
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_thumbnail_test.py"
            "$<TARGET_FILE:ui_shellthumbnail_test>" "$<TARGET_FILE:tconverter>"
            "$<TARGET_FILE:toonzpreview>" "${CMAKE_SOURCE_DIR}/../../stuff")
    set_tests_properties(ui_shellthumbnail PROPERTIES TIMEOUT 210)
endif()
