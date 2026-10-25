add_executable(tconverter
    tconverter.cpp
)

target_link_libraries(tconverter
    Qt${QT_VERSION_MAJOR}::Core
    toonzlib
    image
)
