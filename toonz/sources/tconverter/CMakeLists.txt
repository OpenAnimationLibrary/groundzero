add_executable(tconverter
    tconverter.cpp
    shellthumbnail.cpp
    shellthumbnail.h
)

target_link_libraries(tconverter
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui
    toonzlib
    image
    toonzqt
    tnzstdfx
    colorfx
)
