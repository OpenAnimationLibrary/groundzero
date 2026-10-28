set(MOC_HEADERS
    controlpointselection.h
    edittoolgadgets.h
    filltool.h
    fullcolorbrushtool.h
    fullcolorfilltool.h
    plastictool.h
    skeletonsubtools.h
    tooloptionscontrols.h
    toonzrasterbrushtool.h
    viewtools.h
    selectiontool.h
    edittool.h
    controlpointeditortool.h
    geometrictool.h
    ../include/tools/imagegrouping.h
    ../include/tools/screenpicker.h
    ../include/tools/toolhandle.h
    ../include/tools/tooloptions.h
)

set(HEADERS
    ${MOC_HEADERS}
    autofill.h
    typetooltext.h
    bluredbrush.h
    hookselection.h
    mypainttoonzbrush.h
    rasterselectiontool.h
    rgbpickertool.h
    rulertool.h
    setsaveboxtool.h
    shifttracetool.h
    stylepickertool.h
    toonzvectorbrushtool.h
    vectorselectiontool.h
    assistants/assistantellipse.h
    assistants/assistantline.h
    assistants/assistantvanishingpoint.h
    ../include/tools/RGBpicker.h
    ../include/tools/artlayer.h
    ../include/tools/bilineardeform.h
    ../include/tools/envelopelayout.h
    ../include/tools/cursormanager.h
    ../include/tools/cursors.h
    ../include/tools/levelselection.h
    ../include/tools/rasterselection.h
    ../include/tools/strokeselection.h
    ../include/tools/vectoralignment.h
    ../include/tools/fluidwidth.h
    ../include/tools/widthprofile.h
    ../include/tools/gradienthandles.h
    ../include/tools/stylepicker.h
    ../include/tools/tool.h
    ../include/tools/toolcommandids.h
    ../include/tools/toolutils.h
    ../include/tools/tooltimer.h
    ../include/tools/keyhistory.h
    ../include/tools/inputstate.h
    ../include/tools/track.h
    ../include/tools/inputmanager.h
    ../include/tools/assistant.h
    ../include/tools/editassistantstool.h
    ../include/tools/replicator.h
    ../include/tools/modifiers/modifierassistants.h
    ../include/tools/modifiers/modifierclone.h
    ../include/tools/modifiers/modifierjitter.h
    ../include/tools/modifiers/modifierline.h
    ../include/tools/modifiers/modifiersegmentation.h
    ../include/tools/modifiers/modifiersimplify.h
    ../include/tools/modifiers/modifiersmooth.h
    ../include/tools/modifiers/modifiertangents.h
    ../include/tools/modifiers/modifiertest.h
    ../include/tools/assistants/guidelineline.h
    ../include/tools/assistants/guidelineellipse.h
)

set(SOURCES
    artlayer.cpp
    autofillpli.cpp
    autofilltlv.cpp
    cursormanager.cpp
    stylepicker.cpp
    tool.cpp
    toolhandle.cpp
    toolutils.cpp
    bendertool.cpp
    bluredbrush.cpp
    controlpointeditortool.cpp
    cuttertool.cpp
    edittool.cpp
    edittoolgadgets.cpp
    filltool.cpp
    fullcolorbrushtool.cpp
    fullcolorerasertool.cpp
    fullcolorfilltool.cpp
    geometrictool.cpp
    hooktool.cpp
    hookselection.cpp
    imagegrouping.cpp
    irontool.cpp
    lassocuttertool.cpp
    levelselection.cpp
    magnettool.cpp
    paintbrushtool.cpp
    pinchtool.cpp
    plastictool.cpp
    plastictool_animate.cpp
    plastictool_build.cpp
    plastictool_meshedit.cpp
    plastictool_rigidity.cpp
    pumptool.cpp
    rastererasertool.cpp
    rastertapetool.cpp
    rasterselectiontool.cpp
    rgbpickertool.cpp
    selectiontool.cpp
    setsaveboxtool.cpp
    skeletonsubtools.cpp
    skeletontool.cpp
    stylepickertool.cpp
    trackertool.cpp
    typetool.cpp
    typetooltext.cpp
    tooloptionscontrols.cpp
    tooloptions.cpp
    vectorerasertool.cpp
    vectorselectiontool.cpp
    vectortapetool.cpp
    viewtools.cpp
    controlpointselection.cpp
    rasterselection.cpp
    strokeselection.cpp
    widthprofilecommands.cpp
    screenpicker.cpp
    fingertool.cpp
    gradienttransformtool.cpp
    rulertool.cpp
    mypainttoonzbrush.cpp
    shifttracetool.cpp
    toonzrasterbrushtool.cpp
    toonzvectorbrushtool.cpp
    tooltimer.cpp
    inputstate.cpp
    track.cpp
    inputmanager.cpp
    assistant.cpp
    replicator.cpp
    modifiers/modifierassistants.cpp
    modifiers/modifierclone.cpp
    modifiers/modifierjitter.cpp
    modifiers/modifierline.cpp
    modifiers/modifiersegmentation.cpp
    modifiers/modifiersimplify.cpp
    modifiers/modifiersmooth.cpp
    modifiers/modifiertangents.cpp
    modifiers/modifiertest.cpp
    assistants/guidelineline.cpp
    assistants/guidelineellipse.cpp
    assistants/assistantellipse.cpp
    assistants/assistantfisheye.cpp
    assistants/assistantline.cpp
    assistants/assistantperspective.cpp
    assistants/assistantvanishingpoint.cpp
    assistants/replicatoraffine.cpp
    assistants/replicatorgrid.cpp
    assistants/replicatorjitter.cpp
    assistants/replicatormirror.cpp
    assistants/replicatorstar.cpp
    editassistantstool.cpp
)

set(RESOURCES tnztools.qrc)

ot_qt_add_resources(SOURCES ${RESOURCES})

ot_qt_wrap_cpp(SOURCES ${MOC_HEADERS})

add_library(tnztools SHARED ${HEADERS} ${SOURCES} ${RESOURCES})

if(WITH_TRANSLATION)
    add_translation(tnztools ${HEADERS} ${SOURCES})
    add_compile_translations_command(tnztools tnztools)
endif()

add_definitions(
    -DTNZTOOLS_EXPORTS
)

if(BUILD_ENV_APPLE)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-install_name,@executable_path/libtnztools.dylib")
    add_dependencies(tnztools tnzcore tnzbase tnzext toonzlib toonzqt)
endif()

message("subdir: tnztools")
message("Bin: " ${CMAKE_CURRENT_BINARY_DIR})

include_directories(
    SYSTEM
    ${SDKROOT}/superlu/SupperLU_4.1/SRC
)
include_directories(
    ../include/tools
)

_find_toonz_library(EXTRA_LIBS "tnzcore;tnzbase;tnzext;toonzlib;toonzqt")

target_link_libraries(tnztools Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::OpenGL ${OT_QT_OPENGLWIDGETS_LIB} ${OT_QT_CORE5COMPAT_LIB} ${GLUT_LIB} ${EXTRA_LIBS} ${GL_LIB} ${MYPAINT_LIB_LDFLAGS})

# Optional regression targets for tool helpers that need no application setup.
if(BUILD_TNZTOOLS_TESTS)
    add_subdirectory(tests)
endif()
