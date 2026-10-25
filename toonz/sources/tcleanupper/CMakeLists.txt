add_executable(tcleanup
    tcleanupper.cpp
)

target_link_libraries(tcleanup
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Widgets
    tfarm
    image
)
