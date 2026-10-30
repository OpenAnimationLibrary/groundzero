set(MOC_HEADERS
    ../include/toonz/fullcolorpalette.h
    ../include/toonz/movierenderer.h
    ../include/toonz/multimediarenderer.h
    ../include/toonz/palettecontroller.h
    ../include/toonz/preferences.h
    ../include/toonz/scriptbinding.h
    ../include/toonz/scriptbinding_centerline_vectorizer.h
    ../include/toonz/scriptbinding_files.h
    ../include/toonz/scriptbinding_image.h
    ../include/toonz/scriptbinding_image_builder.h
    ../include/toonz/scriptbinding_level.h
    ../include/toonz/scriptbinding_outline_vectorizer.h
    ../include/toonz/scriptbinding_rasterizer.h
    ../include/toonz/scriptbinding_renderer.h
    ../include/toonz/scriptbinding_scene.h
    ../include/toonz/scriptbinding_stageobject.h
    ../include/toonz/scriptbinding_toonz_raster_converter.h
    ../include/toonz/scriptengine.h
    ../include/toonz/stylemanager.h
    ../include/toonz/tcenterlinevectorizer.h
    ../include/toonz/tcolumnhandle.h
    ../include/toonz/tframehandle.h
    ../include/toonz/tfxhandle.h
    ../include/toonz/tobjecthandle.h
    ../include/toonz/tonionskinmaskhandle.h
    ../include/toonz/tpalettehandle.h
    ../include/toonz/tscenehandle.h
    ../include/toonz/txsheethandle.h
    ../include/toonz/txshlevel.h
    ../include/toonz/txshlevelhandle.h
    ../include/toonz/txshsimplelevel.h
    ../include/toonz/txshsoundcolumn.h
)

set(HEADERS
    ${MOC_HEADERS}
    audioscrubplayer.h
    autoadjust.h
    autopos.h
    cleanupcommon.h
    cleanuppalette.h
    exrlevelloader.h
    exrlevelsource.h
    imagebuilders.h
    ../include/toonz/txshexrlevel.h
    ../include/toonz/lut3d.h
    ../include/toonz/particlecache.h
    skeletonlut.h
    tcenterlinevectP.h
    texturemanager.h
    xshhandlemanager.h
    sandor_fxs/BlurMatrix.h
    sandor_fxs/CIL.h
    sandor_fxs/CallCircle.h
    sandor_fxs/CallParam.h
    sandor_fxs/EraseContour.h
    sandor_fxs/InputParam.h
    sandor_fxs/Params.h
    sandor_fxs/Pattern.h
    sandor_fxs/PatternMapParam.h
    sandor_fxs/PatternPosition.h
    sandor_fxs/SDef.h
    sandor_fxs/SDirection.h
    sandor_fxs/SError.h
    sandor_fxs/STColSelPic.h
    sandor_fxs/STPic.h
    sandor_fxs/YOMBInputParam.h
    sandor_fxs/YOMBParam.h
    sandor_fxs/blend.h
    sandor_fxs/calligraph.h
    sandor_fxs/patternmap.h
    sandor_fxs/toonz4_6staff.h
    ../include/convert2tlv.h
    ../include/thirdparty.h
    ../include/orientation.h
    ../include/toonz/Naa2TlvConverter.h
    ../include/toonz/abrparser.h
    ../include/toonz/autoclose.h
    ../include/toonz/boardsettings.h
    ../include/toonz/captureparameters.h
    ../include/toonz/childstack.h
    ../include/toonz/cleanupcolorstyles.h
    ../include/toonz/cleanupparameters.h
    ../include/toonz/columnfan.h
    ../include/toonz/controlpointobserver.h
    ../include/toonz/customvectorbrushexporter.h
    ../include/toonz/currentimage.h
    ../include/toonz/doubleparamcmd.h
    ../include/toonz/dpiscale.h
    ../include/toonz/fill.h
    ../include/toonz/fxcommand.h
    ../include/toonz/fxdag.h
    ../include/toonz/glrasterpainter.h
    ../include/toonz/hook.h
    ../include/toonz/ikengine.h
    ../include/toonz/ikjacobian.h
    ../include/toonz/iknode.h
    ../include/toonz/ikskeleton.h
    ../include/toonz/imagelocation.h
    ../include/toonz/imagemanager.h
    ../include/toonz/imagepainter.h
    ../include/toonz/imagestyles.h
    ../include/toonz/levelproperties.h
    ../include/toonz/levelset.h
    ../include/toonz/levelupdater.h
    ../include/toonz/logger.h
    ../include/toonz/motionpreset.h
    ../include/toonz/mypaint.h
    ../include/toonz/mypaintbrushstyle.h
    ../include/toonz/mypainthelpers.hpp
    ../include/toonz/namebuilder.h
    ../include/toonz/observer.h
    ../include/toonz/onionskinmask.h
    ../include/toonz/ghostfade.h
    ../include/toonz/motionarc.h
    ../include/toonz/palettecmd.h
    ../include/toonz/plasticdeformerfx.h
    ../include/toonz/poselibrary.h
    ../include/toonz/rasterbrush.h
    ../include/toonz/rasterstrokegenerator.h
    ../include/toonz/scenechecker.h
    ../include/toonz/scenefx.h
    ../include/toonz/sceneproperties.h
    ../include/toonz/sceneresources.h
    ../include/toonz/skeleton.h
    ../include/toonz/stage.h
    ../include/toonz/stage2.h
    ../include/toonz/stageobjectutil.h
    ../include/toonz/stageplayer.h
    ../include/toonz/stagevisitor.h
    ../include/toonz/studiopalette.h
    ../include/toonz/studiopalettecmd.h
    ../include/toonz/tapplication.h
    ../include/toonz/targetcolors.h
    ../include/toonz/tbinarizer.h
    ../include/toonz/tbonedeformer.h
    ../include/toonz/tcamera.h
    ../include/toonz/tcleanupper.h
    ../include/toonz/tcolumnfx.h
    ../include/toonz/tcolumnfxset.h
    ../include/toonz/contouroutline.h
    ../include/toonz/lottieexporter.h
    ../include/toonz/lottiemath.h
    ../include/toonz/tcurvedeformer.h
    ../include/toonz/tdeformer.h
    ../include/toonz/tdeformerstack.h
    ../include/toonz/tfreeformdeformer.h
    ../include/toonz/tdistort.h
    ../include/toonz/textureutils.h
    ../include/toonz/tlog.h
    ../include/toonz/toonzfolders.h
    ../include/toonz/toonzimageutils.h
    ../include/toonz/toonzscene.h
    ../include/toonz/tpinnedrangeset.h
    ../include/toonz/tproject.h
    ../include/toonz/trasterimageutils.h
    ../include/toonz/tshapeawaredeformer.h
    ../include/toonz/tstageobject.h
    ../include/toonz/tstageobjectcmd.h
    ../include/toonz/tstageobjectid.h
    ../include/toonz/tstageobjectkeyframe.h
    ../include/toonz/tstageobjectspline.h
    ../include/toonz/tstageobjecttree.h
    ../include/toonz/timeremap.h
    ../include/toonz/columncoloreffect.h
    ../include/toonz/timeremapcmd.h
    ../include/toonz/ttilesaver.h
    ../include/toonz/ttileset.h
    ../include/toonz/tvectorimageutils.h
    ../include/toonz/txshcell.h
    ../include/toonz/txshchildlevel.h
    ../include/toonz/txshcolumn.h
    ../include/toonz/txsheet.h
    ../include/toonz/txsheetexpr.h
    ../include/toonz/txshlevelcolumn.h
    ../include/toonz/txshleveltypes.h
    ../include/toonz/txshmeshcolumn.h
    ../include/toonz/txshnoteset.h
    ../include/toonz/txshpalettecolumn.h
    ../include/toonz/txshpalettelevel.h
    ../include/toonz/txshsoundlevel.h
    ../include/toonz/txshsoundtextcolumn.h
    ../include/toonz/txshsoundtextlevel.h
    ../include/toonz/txshzeraryfxcolumn.h
    ../include/toonz/txshzeraryfxlevel.h
    ../include/toonz/vectorizerparameters.h
    ../include/toutputproperties.h
    ../include/toonz/preferencesitemids.h
    ../include/toonz/txsheetcolumnchange.h
    ../include/toonz/expressionreferencemonitor.h
    ../include/toonz/filepathproperties.h
    ../include/toonz/navigationtags.h
)

set(SOURCES
    particlecache.cpp
    abrparser.cpp
    autoadjust.cpp
    autoclose.cpp
    autopos.cpp
    captureparameters.cpp
    childstack.cpp
    cleanupcolorstyles.cpp
    cleanuppalette.cpp
    cleanupparameters.cpp
    columnfan.cpp
    convert2tlv.cpp
    customvectorbrushexporter.cpp
    dpiscale.cpp
    exrlevelloader.cpp
    exrlevelsource.cpp
    txshexrlevel.cpp
    fill.cpp
    fillutil.cpp
    fullcolorpalette.cpp
    fxdag.cpp
    glrasterpainter.cpp
    hook.cpp
    ikengine.cpp
    ikjacobian.cpp
    iknode.cpp
    ikskeleton.cpp
    imagebuilders.cpp
    imagelocation.cpp
    imagemanager.cpp
    imagepainter.cpp
    imagestyles.cpp
    levelproperties.cpp
    levelset.cpp
    levelupdater.cpp
    lut3d.cpp
    logger.cpp
    motionpreset.cpp
    movierenderer.cpp
    multimediarenderer.cpp
    mypaintbrushstyle.cpp
    namebuilder.cpp
    Naa2TlvConverter.cpp
    observer.cpp
    onionskinmask.cpp
    motionarc.cpp
	orientation.cpp
    outputproperties.cpp
    poselibrary.cpp
    preferences.cpp
    rasterbrush.cpp
    rasterstrokegenerator.cpp
    scenecompatibility.cpp
    scenechecker.cpp
    scenefx.cpp
    sceneproperties.cpp
    sceneresources.cpp
    scriptbinding.cpp
    scriptbinding_centerline_vectorizer.cpp
    scriptbinding_files.cpp
    scriptbinding_image.cpp
    scriptbinding_image_builder.cpp
    scriptbinding_level.cpp
    scriptbinding_outline_vectorizer.cpp
    scriptbinding_rasterizer.cpp
    scriptbinding_renderer.cpp
    scriptbinding_scene.cpp
    scriptbinding_stageobject.cpp
    scriptbinding_toonz_raster_converter.cpp
    scriptengine.cpp
    skeleton.cpp
    skeletonlut.cpp
    stage.cpp
    stage2.cpp
    stageobjectutil.cpp
    stageplayer.cpp
    stagevisitor.cpp
    strokegenerator.cpp
    studiopalette.cpp
    stylemanager.cpp
    targetcolors.cpp
    tbonedeformer.cpp
    tcamera.cpp
    tcenterlineadjustments.cpp
    tcenterlinecolors.cpp
    contouroutline.cpp
    lottieexporter.cpp
    tcenterlinepolygonizer.cpp
    tcenterlineskeletonizer.cpp
    tcenterlinetostrokes.cpp
    tcenterlinevectorizer.cpp
    tcleanupper.cpp
    tcolumnfx.cpp
    tcolumnfxset.cpp
    tcurvedeformer.cpp
    tdeformer.cpp
    tdeformerstack.cpp
    tdistort.cpp
    tfreeformdeformer.cpp
    texturemanager.cpp
    thirdparty.cpp
    tlog.cpp
    tnewoutlinevectorize.cpp
    toonzfolders.cpp
    toonzimageutils.cpp
    toonzscene.cpp
    toutlinevectorizer.cpp
    tpinnedrangeset.cpp
    tproject.cpp
    tshapeawaredeformer.cpp
    trasterimageutils.cpp
    trastercentroid.cpp
    tstageobject.cpp
    tstageobjectspline.cpp
    tstageobjecttree.cpp
    timeremapcmd.cpp
    ttilesaver.cpp
    ttileset.cpp
    tvectorimageutils.cpp
    txshcell.cpp
    txshchildlevel.cpp
    txshcolumn.cpp
    txsheet.cpp
    txsheetexpr.cpp
    txshlevel.cpp
    txshlevelcolumn.cpp
    txshnoteset.cpp
    txshpalettecolumn.cpp
    txshpalettelevel.cpp
    txshsimplelevel.cpp
    txshsoundcolumn.cpp
    txshsoundlevel.cpp
    txshsoundtextcolumn.cpp
    txshsoundtextlevel.cpp
    txshzeraryfxcolumn.cpp
    txshzeraryfxlevel.cpp
    xshhandlemanager.cpp
    vectorizerparameters.cpp
    sandor_fxs/blend.cpp
    sandor_fxs/BlurMatrix.cpp
    sandor_fxs/CIL.cpp
    sandor_fxs/CallCircle.cpp
    sandor_fxs/CallParam.cpp
    sandor_fxs/EraseContour.cpp
    sandor_fxs/InputParam.cpp
    sandor_fxs/Params.cpp
    sandor_fxs/Pattern.cpp
    sandor_fxs/PatternMapParam.cpp
    sandor_fxs/PatternPosition.cpp
    sandor_fxs/SDirection.cpp
    sandor_fxs/SError.cpp
    sandor_fxs/STColSelPic.cpp
    sandor_fxs/STPic.cpp
    sandor_fxs/YOMBInputParam.cpp
    sandor_fxs/YOMBParam.cpp
    sandor_fxs/calligraph.cpp
    sandor_fxs/patternmap.cpp
    sandor_fxs/toonz4_6staff.cpp
    tcolumnhandle.cpp
    tframehandle.cpp
    tfxhandle.cpp
    tobjecthandle.cpp
    tonionskinmaskhandle.cpp
    tpalettehandle.cpp
    tscenehandle.cpp
    txsheethandle.cpp
    txshlevelhandle.cpp
    doubleparamcmd.cpp
    fxcommand.cpp
    palettecmd.cpp
    palettecontroller.cpp
    studiopalettecmd.cpp
    tstageobjectcmd.cpp
    tbinarizer.cpp
    plasticdeformerfx.cpp
    txshmeshcolumn.cpp
    textureutils.cpp
    boardsettings.cpp
    filepathproperties.cpp
    navigationtags.cpp
)

if(BUILD_TARGET_WIN)
    set(SOURCES ${SOURCES}
        avicodecrestrictions.cpp
    )
endif()

# Python scripting (the embedded interpreter and the opentoonz module)
if(WITH_PYTHON)
    set(MOC_HEADERS ${MOC_HEADERS} ../include/toonz/pythonengine.h)
    set(HEADERS ${HEADERS} ../include/toonz/pythonengine.h pythonbinding.h)
    set(SOURCES ${SOURCES} pythonbinding.cpp pythonengine.cpp)
endif()

ot_qt_wrap_cpp(SOURCES ${MOC_HEADERS})

add_library(toonzlib SHARED ${HEADERS} ${SOURCES})

if(WITH_TRANSLATION)
    add_translation(toonzlib ${HEADERS} ${SOURCES})
    add_compile_translations_command(toonzlib toonzlib)
endif()

if(BUILD_ENV_APPLE)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-install_name,@executable_path/libtoonzlib.dylib")
    add_dependencies(toonzlib tnzcore tnzbase tnzext)
endif()

# Only the library defines its exported symbols. Test executables consume
# the DLL and must see dllimport declarations on Windows.
target_compile_definitions(toonzlib PRIVATE TOONZLIB_EXPORTS)

message("subdir: toonzlib")
message("Bin: " ${CMAKE_CURRENT_BINARY_DIR})

if(GLEW_FOUND)
    include_directories(${GLEW_INCLUDE_DIRS})
else()
    include_directories(${SDKROOT}/glew/glew-1.9.0/include)
endif()

include_directories(
    SYSTEM
    ${SDKROOT}/libusb/libusb-1.0.9/include
)
include_directories(
    ../toonzfarm/include
)

if(BUILD_ENV_MSVC)
    target_link_libraries(toonzlib
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::OpenGL ${OT_QT_OPENGLWIDGETS_LIB} Qt${QT_VERSION_MAJOR}::Qml Qt${QT_VERSION_MAJOR}::Multimedia ${OT_QT_CORE5COMPAT_LIB}
        ${GLUT_LIB} ${GL_LIB} ${MYPAINT_LIB_LDFLAGS} ${GLEW_LIB} vfw32.lib
        tnzcore tnzbase tnzext
    )
elseif(BUILD_ENV_APPLE)
    set(EXTRA_LIBS
        "$<TARGET_FILE:tnzcore>"
	    "$<TARGET_FILE:tnzbase>"
	    "$<TARGET_FILE:tnzext>"
        ${USB_LIB}
        ${MYPAINT_LIB_LDFLAGS}
    )

    target_link_libraries(toonzlib Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::OpenGL ${OT_QT_OPENGLWIDGETS_LIB} Qt${QT_VERSION_MAJOR}::Qml Qt${QT_VERSION_MAJOR}::Multimedia ${OT_QT_CORE5COMPAT_LIB} ${GLUT_LIB} ${GL_LIB} ${GLEW_LIB} ${EXTRA_LIBS})
elseif(BUILD_ENV_UNIXLIKE)
    _find_toonz_library(EXTRA_LIBS "tnzcore;tnzbase;tnzext")

    if(BUILD_TARGET_WIN)
        set(EXTRA_LIBS ${EXTRA_LIBS} -lvfw32)
    endif()

    target_link_libraries(toonzlib Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::OpenGL ${OT_QT_OPENGLWIDGETS_LIB} Qt${QT_VERSION_MAJOR}::Qml Qt${QT_VERSION_MAJOR}::Multimedia ${OT_QT_CORE5COMPAT_LIB} ${GLUT_LIB} ${GL_LIB} ${GLEW_LIB} ${EXTRA_LIBS} ${MYPAINT_LIB_LDFLAGS})
endif()

target_link_libraries(toonzlib OpenEXR::OpenEXR)
target_link_libraries(toonzlib Partio::Partio)
if(BUILD_ENV_MSVC)
    target_include_directories(toonzlib PRIVATE "${SDKROOT}/zlib-1.2.8")
    target_link_libraries(toonzlib ${Z_LIB})
else()
    target_link_libraries(toonzlib ZLIB::ZLIB)
endif()

option(BUILD_PARTICLE_CACHE_TESTS "Build particle cache integration regressions" ${BUILD_TESTS})
if(BUILD_PARTICLE_CACHE_TESTS)
    add_executable(particlecache_test tests/particlecache_test.cpp)
    target_link_libraries(particlecache_test PRIVATE toonzlib tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME particlecache_core COMMAND particlecache_test)
    add_executable(particlecache_formats_test tests/particlecache_formats_test.cpp)
    target_link_libraries(particlecache_formats_test PRIVATE toonzlib tnzcore Partio::Partio Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME particlecache_formats COMMAND particlecache_formats_test)
    set_tests_properties(particlecache_core particlecache_formats PROPERTIES TIMEOUT 120)
endif()

if(WITH_PYTHON)
    target_link_libraries(toonzlib pybind11::embed)
endif()

# Optional regression target for the generic deformers (tdeformer.h).
option(BUILD_DEFORMER_TESTS "Build the deformer regression test" ${BUILD_TESTS})
if(BUILD_DEFORMER_TESTS)
    add_executable(deformer_test tests/deformer_test.cpp)
    target_link_libraries(deformer_test PRIVATE toonzlib tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME deformer COMMAND deformer_test)
endif()

# Optional regression target for the Photoshop brush (ABR) reader.
option(BUILD_ABR_TESTS "Build the ABR brush reader test" ${BUILD_TESTS})
if(BUILD_ABR_TESTS)
    add_executable(abrparser_test tests/abrparser_test.cpp)
    target_link_libraries(abrparser_test PRIVATE toonzlib tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME abrparser COMMAND abrparser_test)
endif()

# Optional regression target for the pure helpers of the time remap (timeremap.h).
option(BUILD_TIMEREMAP_TESTS "Build the time remap regression test" ${BUILD_TESTS})
if(BUILD_TIMEREMAP_TESTS)
    add_executable(timeremap_test tests/timeremap_test.cpp)
    target_link_libraries(timeremap_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME timeremap COMMAND timeremap_test)
endif()

# Optional regression targets for the Texture Atlas export: the rectangle
# packer (atlaspacker.h) and the JSON builder (atlasjson.h), both header only.
option(BUILD_ATLAS_TESTS "Build the Texture Atlas packer and JSON regression tests" ${BUILD_TESTS})
if(BUILD_ATLAS_TESTS)
    add_executable(atlaspacker_test tests/atlaspacker_test.cpp)
    add_test(NAME atlaspacker COMMAND atlaspacker_test)

    add_executable(atlasjson_test tests/atlasjson_test.cpp)
    target_link_libraries(atlasjson_test PRIVATE Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME atlasjson COMMAND atlasjson_test)
endif()

# Optional regression target for the aspect ratio guide math (cameraguide.h).
option(BUILD_CAMERAGUIDE_TESTS "Build the aspect ratio guide regression test" ${BUILD_TESTS})
if(BUILD_CAMERAGUIDE_TESTS)
    add_executable(cameraguide_test tests/cameraguide_test.cpp)
    target_link_libraries(cameraguide_test PRIVATE tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME cameraguide COMMAND cameraguide_test)
endif()

# Optional regression target for the Pegbar Guide layout (pegbarguide.h).
option(BUILD_PEGBARGUIDE_TESTS "Build the pegbar guide regression test" ${BUILD_TESTS})
if(BUILD_PEGBARGUIDE_TESTS)
    add_executable(pegbarguide_test tests/pegbarguide_test.cpp)
    target_link_libraries(pegbarguide_test PRIVATE toonzlib tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME pegbarguide COMMAND pegbarguide_test)
endif()

# Optional regression target for the motion arcs (motionarc.h): the math on
# the samples and the sampling of a scene in memory.
option(BUILD_MOTIONARC_TESTS "Build the motion arcs regression test" ${BUILD_TESTS})
if(BUILD_MOTIONARC_TESTS)
    add_executable(motionarc_test tests/motionarc_test.cpp)
    target_link_libraries(motionarc_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
    add_test(NAME motionarc COMMAND motionarc_test)
endif()

# Optional regression target for the Outline View of the columns: the colour
# palette (outlinecolors.h) and the column status bit.
option(BUILD_OUTLINEVIEW_TESTS "Build the column Outline View regression test" ${BUILD_TESTS})
if(BUILD_OUTLINEVIEW_TESTS)
    add_executable(outlineview_test tests/outlineview_test.cpp)
    target_link_libraries(outlineview_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME outlineview COMMAND outlineview_test)
endif()

# Optional regression target for the camera's depth channels, Focus Z and
# Aperture: saved and keyed only when used, and read by expressions.
option(BUILD_CAMERADEPTH_TESTS "Build the camera depth channels regression test" ${BUILD_TESTS})
if(BUILD_CAMERADEPTH_TESTS)
    add_executable(cameradepth_test tests/cameradepth_test.cpp)
    target_link_libraries(cameradepth_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME cameradepth COMMAND cameradepth_test)
endif()

# Optional regression target for the Multiplane Depth (multiplanedepth.h): the
# math, the scene setting, and the blur and haze inserted per column by the
# render. It links the standard FX for the Blur FX.
option(BUILD_MULTIPLANEDEPTH_TESTS "Build the Multiplane Depth regression test" ${BUILD_TESTS})
if(BUILD_MULTIPLANEDEPTH_TESTS)
    add_executable(multiplanedepth_test tests/multiplanedepth_test.cpp)
    target_link_libraries(multiplanedepth_test PRIVATE toonzlib tnzstdfx tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
    add_test(NAME multiplanedepth COMMAND multiplanedepth_test)
endif()

# Optional regression target for the Colour Script (palettetransform.h): the
# hue, saturation and lightness shift and the render data applying it.
option(BUILD_COLOURSCRIPT_TESTS "Build the Colour Script regression test" ${BUILD_TESTS})
if(BUILD_COLOURSCRIPT_TESTS)
    add_executable(colourscript_test tests/colourscript_test.cpp)
    target_link_libraries(colourscript_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME colourscript COMMAND colourscript_test)
endif()

# Optional regression target for the motion presets (motionpreset.h). It also
# loads the stock presets shipped in stuff/library/motionpresets.
option(BUILD_MOTIONPRESET_TESTS "Build the motion preset regression test" ${BUILD_TESTS})
if(BUILD_MOTIONPRESET_TESTS)
    get_filename_component(MOTIONPRESETS_STOCK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/library/motionpresets" ABSOLUTE)
    add_executable(motionpreset_test tests/motionpreset_test.cpp)
    target_link_libraries(motionpreset_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME motionpreset COMMAND motionpreset_test ${MOTIONPRESETS_STOCK_DIR})
endif()

# Optional regression target for the matching of Rig Mapping (rigmapping.h,
# header only).
option(BUILD_RIGMAPPING_TESTS "Build the rig mapping regression test" ${BUILD_TESTS})
if(BUILD_RIGMAPPING_TESTS)
    add_executable(rigmapping_test tests/rigmapping_test.cpp)
    add_test(NAME rigmapping COMMAND rigmapping_test)
endif()

# Optional regression target for the Ghost Fade of the onion skin
# (ghostfade.h, header only).
option(BUILD_GHOSTFADE_TESTS "Build the onion skin Ghost Fade regression test" ${BUILD_TESTS})
if(BUILD_GHOSTFADE_TESTS)
    add_executable(ghostfade_test tests/ghostfade_test.cpp)
    add_test(NAME ghostfade COMMAND ghostfade_test)
endif()

# Optional regression target for the per-column Color Effect (the math of
# columncoloreffect.h and the column fx parameters, saved only when used).
option(BUILD_COLUMNCOLOREFFECT_TESTS "Build the column Color Effect regression test" ${BUILD_TESTS})
if(BUILD_COLUMNCOLOREFFECT_TESTS)
    add_executable(columncoloreffect_test tests/columncoloreffect_test.cpp)
    target_link_libraries(columncoloreffect_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME columncoloreffect COMMAND columncoloreffect_test)
endif()

# Optional regression target for the item names and the pose set merge of
# the shared Library (libraryitems.h).
option(BUILD_LIBRARYITEMS_TESTS "Build the shared Library helpers regression test" ${BUILD_TESTS})
if(BUILD_LIBRARYITEMS_TESTS)
    add_executable(libraryitems_test tests/libraryitems_test.cpp)
    target_link_libraries(libraryitems_test PRIVATE tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME libraryitems COMMAND libraryitems_test)
endif()

# Optional regression target for the export to an aspect ratio
# (TOutputProperties and its scene file tag).
option(BUILD_ASPECTEXPORT_TESTS "Build the aspect ratio export regression test" ${BUILD_TESTS})
if(BUILD_ASPECTEXPORT_TESTS)
    add_executable(aspectexport_test tests/aspectexport_test.cpp)
    target_link_libraries(aspectexport_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
    add_test(NAME aspectexport COMMAND aspectexport_test)
endif()

# Optional regression target for the brush stroke to outline shape conversion
# (contouroutline.h).
option(BUILD_CONTOUR_TESTS "Build the contour outline regression test" ${BUILD_TESTS})
if(BUILD_CONTOUR_TESTS OR BUILD_UI_PORT_TESTS)
    add_executable(contour_outline_test tests/contour_outline_test.cpp)
    target_link_libraries(contour_outline_test PRIVATE toonzlib tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME ui_vector_contour_outline COMMAND contour_outline_test)
endif()

# Optional regression target for the Lottie export maths (lottiemath.h,
# header-only helpers).
option(BUILD_LOTTIEMATH_TESTS "Build the Lottie export math test" ${BUILD_TESTS})
if(BUILD_LOTTIEMATH_TESTS)
    add_executable(lottiemath_test tests/lottiemath_test.cpp)
    target_link_libraries(lottiemath_test PRIVATE Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME lottiemath COMMAND lottiemath_test)
endif()

# Optional regression target for the Lottie export of a scene built in memory
# (lottieexporter.h).
option(BUILD_LOTTIEEXPORT_TESTS "Build the Lottie export regression test" ${BUILD_TESTS})
if(BUILD_LOTTIEEXPORT_TESTS)
    add_executable(lottieexport_test tests/lottieexport_test.cpp)
    target_link_libraries(lottieexport_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
    add_test(NAME lottieexport COMMAND lottieexport_test)
endif()

# Optional regression target for the compressed scene file format (tstream.h).
if(BUILD_SEEXPR_TESTS)
    add_executable(seexpr_animation_test tests/seexpranimation_test.cpp)
    target_link_libraries(seexpr_animation_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core)
    if(NOT WIN32)
        target_link_libraries(seexpr_animation_test PRIVATE Threads::Threads)
    endif()
    add_test(NAME seexpr_animation COMMAND seexpr_animation_test)
endif()

# Optional regression target for the compressed scene file format (tstream.h).
option(BUILD_STREAM_TESTS "Build the compressed stream round-trip test" ${BUILD_TESTS})
if(BUILD_STREAM_TESTS)
    add_executable(compressed_stream_test tests/compressed_stream_test.cpp)
    target_link_libraries(compressed_stream_test PRIVATE tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME compressed_stream COMMAND compressed_stream_test)
endif()

# Optional regression target for the scene checker (scenechecker.h): each
# check on scenes built in memory, with a case that finds the issue and one
# that does not.
option(BUILD_SCENECHECKER_TESTS "Build the scene checker regression test" ${BUILD_TESTS})
if(BUILD_SCENECHECKER_TESTS)
    add_executable(scenechecker_test tests/scenechecker_test.cpp)
    target_link_libraries(scenechecker_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
    add_test(NAME scenechecker COMMAND scenechecker_test)
endif()

if(BUILD_RELIABILITY_TESTS)
    add_executable(save_transaction_test tests/save_transaction_test.cpp)
    target_link_libraries(save_transaction_test PRIVATE
        toonzlib tnzbase tnzcore image Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
    add_dependencies(save_transaction_test lzocompress lzodecompress)
    if(BUILD_ENV_APPLE)
        # TLV compression finds these helpers beside the running executable.
        # macOS test binaries live outside the application bundle.
        add_custom_command(TARGET save_transaction_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:lzocompress> $<TARGET_FILE:lzodecompress>
                $<TARGET_FILE_DIR:save_transaction_test>
            VERBATIM)
    endif()
    add_test(NAME save_transaction COMMAND save_transaction_test)
    set_tests_properties(save_transaction PROPERTIES TIMEOUT 120)

    add_executable(movierenderqueue_test tests/movierenderqueue_test.cpp)
    add_test(NAME movierenderqueue COMMAND movierenderqueue_test)

    add_executable(levelupdater_abort_test tests/levelupdater_abort_test.cpp)
    target_link_libraries(levelupdater_abort_test PRIVATE
        toonzlib tnzbase tnzcore image Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
    add_test(NAME levelupdater_abort COMMAND levelupdater_abort_test)
    set_tests_properties(levelupdater_abort PROPERTIES TIMEOUT 120)
endif()

# Optional benchmark of scene save and load (master plan E6, see
# doc/scene_io_profile.md). It prints timings and fails on a failed phase or
# a round-trip mismatch.
option(BUILD_SCENEBENCH_TESTS "Build the scene save and load benchmark" ${BUILD_TESTS})
if(BUILD_SCENEBENCH_TESTS)
    add_executable(scenebench_test tests/scenebench_test.cpp)
    target_link_libraries(scenebench_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
    add_test(NAME scenebench COMMAND scenebench_test --repeat 3)
    set_tests_properties(scenebench PROPERTIES TIMEOUT 900)
endif()

if(BUILD_SCRIPT_TESTS)
    add_executable(scriptengine_test tests/scriptengine_test.cpp)
    target_link_libraries(scriptengine_test PRIVATE toonzlib tnzcore Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Qml)
    add_test(NAME scriptengine COMMAND scriptengine_test)
    set_tests_properties(scriptengine PROPERTIES TIMEOUT 300)
endif()

if(BUILD_SCRIPT_TESTS AND WITH_PYTHON)
    add_executable(pythonengine_test tests/pythonengine_test.cpp)
    target_link_libraries(pythonengine_test PRIVATE toonzlib tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME pythonengine COMMAND pythonengine_test)
    set_tests_properties(pythonengine PROPERTIES TIMEOUT 300)
endif()

# compat_regression: begin
# Runtime compatibility tool (see doc/compat_regression.md): it writes what
# it loads, saves and renders, and the Compat CI job compares its outputs with
# those of the same tool built from edition. Not a ctest test. The CI job
# appends this block, up to the end marker, to the CMakeLists.txt of the
# edition tree, so it must stay valid there.
option(BUILD_COMPAT_REGRESSION "Build the compat_regression comparison tool" ${BUILD_TESTS})
if(BUILD_COMPAT_REGRESSION)
    add_executable(compat_regression tests/compat_regression.cpp)
    target_link_libraries(compat_regression PRIVATE toonzlib tnzbase tnzcore tnzext image tnzstdfx colorfx Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
endif()
# compat_regression: end

# Real vector tween/persistence regressions for the artist morphing workflow.
option(BUILD_MORPHWORKFLOW_TESTS "Build native vector morph workflow tests" ${BUILD_TESTS})
if(BUILD_MORPHWORKFLOW_TESTS OR BUILD_UI_PORT_TESTS)
    add_executable(morphworkflow_test tests/morphworkflow_test.cpp)
    target_link_libraries(morphworkflow_test PRIVATE toonzlib tnzbase tnzcore image Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
    add_test(NAME ui_vector_morphworkflow COMMAND morphworkflow_test)
endif()

# Native Cast indexing/persistence tests share the UI qualification gate.
if(BUILD_UI_PORT_TESTS OR BUILD_TESTS)
    add_executable(castworkflow_test tests/castworkflow_test.cpp)
    target_link_libraries(castworkflow_test PRIVATE toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui)
    add_test(NAME castworkflow COMMAND castworkflow_test)
endif()
