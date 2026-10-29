set(HEADERS
    seexprfx.h
    ../include/stdfx/shaderfx.h
    ../include/stdfx/shaderinterface.h
    ../include/stdfx/shadingcontext.h
    deformerfxcommon.h
    deformerparams.h
    gradients.h
    hsvutil.h
    offscreengl.h
    particles.h
    particlesengine.h
    particlesfx.h
    particlesmanager.h
    perlinnoise.h
    pins.h
    stdfx.h
    celsmoothing.h
    t3dsource.h
    ../include/tfxaovsource.h
    texturefxP.h
    warp.h
    motionawarebasefx.h
    igs_color_blend.h
    igs_color_rgb_hls.h
    igs_color_rgb_hsv.h
    igs_density.h
    igs_fog.h
    igs_gaussian_blur.h
    igs_hls_add.h
    igs_hls_adjust.h
    igs_hls_noise.h
    igs_hls_noise_in_camera.h
    igs_hsv_add.h
    igs_hsv_adjust.h
    igs_hsv_noise.h
    igs_hsv_noise_in_camera.h
    igs_ifx_common.h
    igs_level_auto.h
    igs_level_auto_in_camera.h
    igs_levels.h
    igs_line_blur.h
    igs_math_random.h
    igs_maxmin.h
    igs_maxmin_getput.h
    igs_maxmin_lens_matrix.h
    igs_maxmin_multithread.h
    igs_maxmin_slrender.h
    igs_median_filter.h
    igs_median_filter_smooth.h
    igs_motion_blur.h
    igs_motion_wind.h
    igs_motion_wind_pixel.h
    igs_motion_wind_table.h
    igs_negate.h
    igs_os_type.h
    igs_perlin_noise.h
    igs_radial_blur.h
    igs_resource_msg_from_err.h
    igs_resource_multithread.h
    igs_resource_sleep.h
    igs_resource_thread.h
    igs_rotate_blur.h
    igs_warp.h
    ino_common.h
    iwa_adjustexposurefx.h
    iwa_directionalblurfx.h
    iwa_gradientwarpfx.h
    iwa_motionblurfx.h
    iwa_particles.h
    iwa_particlesengine.h
    iwa_particlesfx.h
    iwa_particlesmanager.h
    iwa_perspectivedistortfx.h
    iwa_spectrumfx.h
    iwa_simplexnoise.h
    iwa_noise1234.h
    iwa_fresnel.h
    iwa_pnperspectivefx.h
    iwa_soapbubblefx.h
    iwa_bokehfx.h
    iwa_timecodefx.h
    iwa_bokehreffx.h
    iwa_textfx.h
    iwa_corridorgradientfx.h
    iwa_spingradientfx.h
    iwa_lineargradientfx.h
    iwa_glarefx.h
    iwa_fractalnoisefx.h
    iwa_rainbow_intensity.h
    iwa_rainbowfx.h
    iwa_bokeh_advancedfx.h
    iwa_bokeh_util.h
    globalcontrollablefx.h
    iwa_floorbumpfx.h
    iwa_tangentflowfx.h
    iwa_flowblurfx.h
    iwa_flowpaintbrushfx.h
    iwa_motionflowfx.h
    naru_graph.h
)

if(OpenCV_FOUND)
    list(APPEND HEADERS iwa_bloomfx.h)
endif()

set(SOURCES
    seexprfx.cpp
    seexprattributefx.cpp
    adjustlevelsfx.cpp
    artcontourfx.cpp
    autopatchfx.cpp
    backlitfx.cpp
    blendtzfx.cpp
    blurfx.cpp
    bodyhighlightfx.cpp
    bonedeformerfx.cpp
    tonehighlightfx.cpp
    lightshadingfx.cpp
    bright_contfx.cpp
    calligraphicfx.cpp
    changecolorfx.cpp
    channelmixerfx.cpp
    cloudsfx.cpp
    colorembossfx.cpp
    colouroverridefx.cpp
    colourscriptfx.cpp
    cornerpinfx.cpp
    curvedeformerfx.cpp
    deformerstackfx.cpp
    despecklefx.cpp
    diamondgradientfx.cpp
    dissolvefx.cpp
    embossfx.cpp
    envelopedeformerfx.cpp
    envelopedeformerplusfx.cpp
    erodilatefx.cpp
    externalpalettefx.cpp
    fourpointsgradientfx.cpp
    freedistortfx.cpp
    freeformdeformerfx.cpp
    shapeawaredeformerfx.cpp
    gammafx.cpp
    glbmodelfx.cpp
    imageplanefx.cpp
    glowfx.cpp
    gradients.cpp
    hsvkeyfx.cpp
    hsvscalefx.cpp
    hsvutil.cpp
    kaleido.cpp
    linearwavefx.cpp
    localblurfx.cpp
    localtransparencyfx.cpp
    lut3dbakefx.cpp
    referencecolormatchfx.cpp
    mosaicfx.cpp
    motionblurfx.cpp
    multitonefx.cpp
    noisefx.cpp
    nothingfx.cpp
    openexraovfx.cpp
    palettefilterfx.cpp
    particles.cpp
    particlecachefx.cpp
    particlebake.cpp
    particlebakeio.cpp
    particlesengine.cpp
    particlesfx.cpp
    particlesmanager.cpp
    perlinnoise.cpp
    perlinnoisefx.cpp
    pins.cpp
    posterizefx.cpp
    premultiplyfx.cpp
    radialblurfx.cpp
    celradialblurfx.cpp
    randomwavefx.cpp
    raylitfx.cpp
    celcolorkeepfx.cpp
    celcolorkeyfx.cpp
    celcolorexpandfx.cpp
    celedgelinesfx.cpp
    celboilfx.cpp
    printprocessfx.cpp
    lightwrapfx.cpp
    edgeextendfx.cpp
    mattechokefx.cpp
    flowdisplacefx.cpp
    alphadistancegradientfx.cpp
    celboundaryblurfx.cpp
    celblur.cpp
    celmotion.cpp
    celnoise.cpp
    celdirectionalmath.cpp
    celdirectionalblurfx.cpp
    celsmootherfx.cpp
    celsmoother2fx.cpp
    rgbkeyfx.cpp
    rgbmcutfx.cpp
    rgbmfadefx.cpp
    rgbmscalefx.cpp
    ripplefx.cpp
    rotationalblurfx.cpp
    saltpeppernoisefx.cpp
    shaderfx.cpp
    shaderinterface.cpp
    shadingcontext.cpp
    sharpenfx.cpp
    solarizefx.cpp
    squaregradientfx.cpp
    stdfx.cpp
    targetspotfx.cpp
    texturefx.cpp
    threepointlightfx.cpp
    tilefx.cpp
    tonecurvefx.cpp
    transform3dfx.cpp
    unmultiplyfx.cpp
    warp.cpp
    warpfx.cpp
    igs_attenuation_distribution.cpp
    igs_color_blend.cpp
    igs_color_rgb_hls.cpp
    igs_color_rgb_hsv.cpp
    igs_density.cpp
    igs_fog.cpp
    igs_gauss_distribution.cpp
    igs_gaussian_blur.cpp
    igs_hls_add.cpp
    igs_hls_adjust.cpp
    igs_hls_noise.cpp
    igs_hls_noise_in_camera.cpp
    igs_hsv_add.cpp
    igs_hsv_adjust.cpp
    igs_hsv_noise.cpp
    igs_hsv_noise_in_camera.cpp
    igs_level_auto.cpp
    igs_level_auto_in_camera.cpp
    igs_levels.cpp
    igs_line_blur.cpp
    igs_math_random.cpp
    igs_maxmin.cpp
    igs_maxmin_lens_matrix.cpp
    igs_maxmin_slrender.cpp
    igs_median_filter.cpp
    igs_median_filter_smooth.cpp
    igs_motion_blur.cpp
    igs_motion_wind.cpp
    igs_motion_wind_pixel.cpp
    igs_motion_wind_table.cpp
    igs_negate.cpp
    igs_perlin_noise.cpp
    igs_radial_blur.cpp
    igs_resource_msg_from_err.cpp
    igs_resource_multithread.cpp
    igs_resource_sleep.cpp
    igs_resource_thread.cpp
    igs_rotate_blur.cpp
    igs_warp_hori.cpp
    igs_warp_vert.cpp
    ino_blend_add.cpp
    ino_blend_color_burn.cpp
    ino_blend_color_dodge.cpp
    ino_blend_cross_dissolve.cpp
    ino_blend_darken.cpp
    ino_blend_darker_color.cpp
    ino_blend_divide.cpp
    ino_blend_hard_light.cpp
    ino_blend_hard_mix.cpp
    ino_blend_lighten.cpp
    ino_blend_lighter_color.cpp
    ino_blend_linear_burn.cpp
    ino_blend_linear_dodge.cpp
    ino_blend_linear_light.cpp
    ino_blend_multiply.cpp
    ino_blend_over.cpp
    ino_blend_overlay.cpp
    ino_blend_pin_light.cpp
    ino_blend_screen.cpp
    ino_blend_soft_light.cpp
    ino_blend_subtract.cpp
    ino_blend_vivid_light.cpp
    ino_blur.cpp
    ino_channel_selector.cpp
    ino_common.cpp
    ino_density.cpp
    ino_fog.cpp
    ino_hls_add.cpp
    ino_hls_adjust.cpp
    ino_hls_noise.cpp
    ino_hsv_add.cpp
    ino_hsv_adjust.cpp
    ino_hsv_noise.cpp
    ino_level_auto.cpp
    ino_level_master.cpp
    ino_level_rgba.cpp
    ino_line_blur.cpp
    ino_maxmin.cpp
    ino_median.cpp
    ino_median_filter.cpp
    ino_motion_blur.cpp
    ino_motion_wind.cpp
    ino_negate.cpp
    ino_pn_clouds.cpp
    ino_radial_blur.cpp
    ino_spin_blur.cpp
    ino_warp_hv.cpp
    iwa_adjustexposurefx.cpp
    iwa_directionalblurfx.cpp
    iwa_gradientwarpfx.cpp
    iwa_motionblurfx.cpp
    iwa_particles.cpp
    iwa_particlesengine.cpp
    iwa_particlesfx.cpp
    iwa_particlesmanager.cpp
    iwa_perspectivedistortfx.cpp
    iwa_spectrumfx.cpp
    iwa_tilefx.cpp
    iwa_simplexnoise.cpp
    iwa_noise1234.cpp
    iwa_pnperspectivefx.cpp
    iwa_soapbubblefx.cpp
    ${SDKROOT}/kiss_fft/kiss_fft.c
    ${SDKROOT}/kiss_fft/kiss_fftnd.c
    iwa_bokehfx.cpp
    iwa_timecodefx.cpp
    iwa_bokehreffx.cpp
    iwa_barreldistortfx.cpp
    iwa_textfx.cpp
    iwa_corridorgradientfx.cpp
    iwa_spingradientfx.cpp
    iwa_lineargradientfx.cpp
    iwa_glarefx.cpp
    starglowfx.cpp
    iwa_fractalnoisefx.cpp
    iwa_rainbowfx.cpp
    iwa_bokeh_advancedfx.cpp
    iwa_bokeh_util.cpp
    iwa_floorbumpfx.cpp
    iwa_tangentflowfx.cpp
    iwa_flowblurfx.cpp
    iwa_flowpaintbrushfx.cpp
    iwa_motionflowfx.cpp
    iwa_smootherfx.cpp
    naru_lazybrush.cpp
)

if(OpenCV_FOUND)
    list(APPEND SOURCES iwa_bloomfx.cpp)
endif()

set(OBJCSOURCES
)

add_library(tnzstdfx SHARED ${HEADERS} ${SOURCES} ${OBJCSOURCES})
add_definitions(
    -D_USRDLL
    -DTNZSTDFX_EXPORTS
)

if(BUILD_ENV_APPLE)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-install_name,@executable_path/libtnzstdfx.dylib")
    add_dependencies(tnzstdfx tnzcore tnzbase toonzlib)
endif()

message("subdir: tnzstdfx")
message("Bin: " ${CMAKE_CURRENT_BINARY_DIR})

if(GLEW_FOUND)
    include_directories(
        SYSTEM
        ${GLEW_INCLUDE_DIRS}
    )
else()
    include_directories(
        SYSTEM
        ${SDKROOT}/glew/glew-1.9.0/include
    )
endif()

include_directories(
    ${SDKROOT}/kiss_fft
)

_find_toonz_library(TNZLIBS "tnzcore;tnzbase;toonzlib")

target_link_libraries(tnzstdfx otglb OpenEXR::OpenEXR Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::OpenGL ${OT_QT_OPENGLWIDGETS_LIB} ${GL_LIB} ${GLEW_LIB} ${TNZLIBS} ${PTHREAD_LIBRARY})

if(BUILD_SEEXPR_TESTS)
    find_package(Threads REQUIRED)
    add_executable(seexpr_fx_test tests/seexprfx_test.cpp)
    target_link_libraries(seexpr_fx_test otglb Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    if(WIN32)
        target_link_libraries(seexpr_fx_test Psapi)
    endif()
    add_test(NAME seexpr_fx COMMAND seexpr_fx_test)
    set_tests_properties(seexpr_fx PROPERTIES TIMEOUT 120)
    add_executable(seexpr_surface_fx_test tests/seexprsurfacefx_test.cpp)
    target_link_libraries(seexpr_surface_fx_test otglb Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} Threads::Threads)
    add_test(NAME seexpr_surface_fx COMMAND seexpr_surface_fx_test)
    set_tests_properties(seexpr_surface_fx PROPERTIES TIMEOUT 120)
    add_custom_target(seexpr_fx_tests DEPENDS seexpr_fx_test seexpr_surface_fx_test)
endif()

if(OpenCV_FOUND)
    target_link_libraries(tnzstdfx ${OpenCV_LIBS})
endif()

# Optional regression target for the production LUT parser and raster FX.
option(BUILD_LUT3D_TESTS "Build the 3D LUT Bake regression executable" ${BUILD_TESTS})
if(BUILD_LUT3D_TESTS)
    add_executable(referencecolormatch_test tests/referencecolormatch_test.cpp)
    target_link_libraries(referencecolormatch_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME referencecolormatch COMMAND referencecolormatch_test)
    add_executable(lut3dbake_test tests/lut3dbake_test.cpp)
    target_link_libraries(lut3dbake_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME lut3dbake COMMAND lut3dbake_test)
endif()

# Optional regression targets for the GLB-backed 3D FX framework.
# Not part of BUILD_TESTS: the GLB/3D features are developed and tested in OT3D.
option(BUILD_GLB_FX_TESTS "Build the 3D FX framework regression executables" OFF)
if(BUILD_GLB_FX_TESTS)
    add_executable(glbmodelfx_test tests/glbmodelfx_test.cpp)
    target_link_libraries(glbmodelfx_test otglb toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_executable(threepointlightfx_test tests/threepointlightfx_test.cpp)
    target_link_libraries(threepointlightfx_test otglb Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_executable(transform3dfx_test tests/transform3dfx_test.cpp)
    target_link_libraries(transform3dfx_test otglb Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME glbmodelfx COMMAND glbmodelfx_test)
    add_test(NAME threepointlightfx COMMAND threepointlightfx_test)
    add_test(NAME transform3dfx COMMAND transform3dfx_test)
    # glbmodelfx_test creates a QApplication; CI machines have no display.
    if(NOT WIN32)
        set_tests_properties(glbmodelfx PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

# Optional regression target that renders standard FX graphs and compares
# them with the reference images in tests/fxrender.
option(BUILD_FXRENDER_TESTS "Build the FX render regression test" ${BUILD_TESTS})
if(BUILD_FXRENDER_TESTS)
    add_executable(fxrender_test tests/fxrender_test.cpp)
    target_link_libraries(fxrender_test tnzstdfx Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME fxrender
             COMMAND fxrender_test ${CMAKE_CURRENT_SOURCE_DIR}/tests/fxrender
                     ${CMAKE_CURRENT_BINARY_DIR}/fxrender-output)
    # Rendering into an offscreen surface still needs a platform plugin that
    # creates OpenGL contexts. Qt's macOS/Windows offscreen plugins cannot
    # replace their native backends for this render test.
    if(APPLE)
        set_tests_properties(fxrender PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
    elseif(WIN32)
        set_tests_properties(fxrender PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
    else()
        set_tests_properties(fxrender PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

# Optional regression target for the pure helpers of the Light Shading FX
# (lightshading.h, header-only).
option(BUILD_LIGHTSHADING_TESTS "Build the Light Shading FX helper test" ${BUILD_TESTS})
if(BUILD_LIGHTSHADING_TESTS)
    add_executable(lightshading_test tests/lightshading_test.cpp)
    add_test(NAME lightshading COMMAND lightshading_test)
endif()

# Optional regression target for multipart, tiled, and channel-mapped EXR AOVs.
if(BUILD_EXR_AOV_FX_TESTS)
    add_executable(openexraovfx_test tests/openexraovfx_test.cpp)
    target_link_libraries(openexraovfx_test OpenEXR::OpenEXR Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME openexraovfx COMMAND openexraovfx_test)
endif()

# Optional regression target for frames rendered as parallel strips.
if(BUILD_FRAME_STRIPS_TESTS)
    add_executable(framestrips_test tests/framestrips_test.cpp)
    target_link_libraries(framestrips_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME framestrips COMMAND framestrips_test)
endif()

# Custom render grids retain the legacy identity path unless explicitly enabled.
if(BUILD_CELFX_TESTS)
    add_executable(celfxrendergrid_test tests/celfxrendergrid_test.cpp)
    target_link_libraries(celfxrendergrid_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_rendergrid COMMAND celfxrendergrid_test)
    set_tests_properties(celfx_rendergrid PROPERTIES
                         ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

# Cel effects use production renderers and native parameter widgets.
if(BUILD_CELFX_TESTS)
    add_executable(celcolorkeep_test tests/celcolorkeep_test.cpp)
    target_link_libraries(celcolorkeep_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_colorkeep COMMAND celcolorkeep_test)
    add_executable(celcolorkeep_ui_test tests/celcolorkeep_ui_test.cpp)
    target_link_libraries(celcolorkeep_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_colorkeep_ui COMMAND celcolorkeep_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_celColorKeepFx.xml)
    add_executable(celcolorkey_test tests/celcolorkey_test.cpp)
    target_link_libraries(celcolorkey_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_colorkey COMMAND celcolorkey_test)
    add_executable(celcolorkey_ui_test tests/celcolorkey_ui_test.cpp)
    target_link_libraries(celcolorkey_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_colorkey_ui COMMAND celcolorkey_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_celColorKeyFx.xml)
    add_executable(celcolorkeymath_test tests/celcolorkeymath_test.cpp)
    add_test(NAME celfx_colorkey_math COMMAND celcolorkeymath_test)
    add_executable(celboundaryblur_test tests/celboundaryblur_test.cpp celblur.cpp celcolorkeyfx.cpp)
    target_link_libraries(celboundaryblur_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_boundaryblur COMMAND celboundaryblur_test)
    add_executable(celboundaryblur_ui_test tests/celboundaryblur_ui_test.cpp celblur.cpp)
    target_link_libraries(celboundaryblur_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_boundaryblur_ui COMMAND celboundaryblur_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_celBoundaryBlurFx.xml)
    add_executable(celdirectionalblur_test tests/celdirectionalblur_test.cpp celmotion.cpp celnoise.cpp celdirectionalmath.cpp)
    target_link_libraries(celdirectionalblur_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_directionalblur COMMAND celdirectionalblur_test)
    add_executable(celdirectionalblur_ui_test tests/celdirectionalblur_ui_test.cpp celmotion.cpp celnoise.cpp celdirectionalmath.cpp)
    target_link_libraries(celdirectionalblur_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_directionalblur_ui COMMAND celdirectionalblur_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_celDirectionalBlurFx.xml)
    add_custom_target(celfx_tests DEPENDS celcolorkeep_test celcolorkeep_ui_test celcolorkey_test celcolorkey_ui_test celcolorkeymath_test celfxrendergrid_test celboundaryblur_test celboundaryblur_ui_test celdirectionalblur_test celdirectionalblur_ui_test)
    add_executable(celradialblur_test tests/celradialblur_test.cpp celmotion.cpp celnoise.cpp)
    target_link_libraries(celradialblur_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_radialblur COMMAND celradialblur_test)
    set_tests_properties(celfx_radialblur PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    add_dependencies(celfx_tests celradialblur_test)
    add_executable(celradialblur_ui_test tests/celradialblur_ui_test.cpp celmotion.cpp celnoise.cpp)
    target_link_libraries(celradialblur_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_radialblur_ui COMMAND celradialblur_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_celRadialBlurFx.xml)
    set_tests_properties(celfx_radialblur_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    add_dependencies(celfx_tests celradialblur_ui_test)
    add_executable(celsmoother_test tests/celsmoother_test.cpp)
    target_link_libraries(celsmoother_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_smoother COMMAND celsmoother_test)
    add_executable(celsmoother_ui_test tests/celsmoother_ui_test.cpp)
    target_link_libraries(celsmoother_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_smoother_ui COMMAND celsmoother_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_celSmootherFx.xml)
    add_dependencies(celfx_tests celsmoother_test celsmoother_ui_test)
    set_tests_properties(celfx_smoother celfx_smoother_ui PROPERTIES
                         ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    set_tests_properties(celfx_colorkeep celfx_colorkeep_ui celfx_colorkey celfx_colorkey_ui celfx_colorkey_math celfx_boundaryblur celfx_boundaryblur_ui celfx_directionalblur celfx_directionalblur_ui PROPERTIES
                         ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

if(BUILD_CELFX_TESTS)
    add_executable(celcolorexpand_test tests/celcolorexpand_test.cpp)
    target_link_libraries(celcolorexpand_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_colorexpand COMMAND celcolorexpand_test)
    add_executable(celcolorexpand_ui_test tests/celcolorexpand_ui_test.cpp)
    target_link_libraries(celcolorexpand_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_colorexpand_ui COMMAND celcolorexpand_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_celColorExpandFx.xml)
    add_dependencies(celfx_tests celcolorexpand_test celcolorexpand_ui_test)
    set_tests_properties(celfx_colorexpand celfx_colorexpand_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

if(BUILD_CELFX_TESTS)
    add_executable(celedgelines_test tests/celedgelines_test.cpp)
    target_link_libraries(celedgelines_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_edgelines COMMAND celedgelines_test)
    add_executable(celedgelines_ui_test tests/celedgelines_ui_test.cpp)
    target_link_libraries(celedgelines_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_edgelines_ui COMMAND celedgelines_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_celEdgeLinesFx.xml)
    add_dependencies(celfx_tests celedgelines_test celedgelines_ui_test)
    set_tests_properties(celfx_edgelines celfx_edgelines_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

if(BUILD_CELFX_TESTS)
    add_executable(celboil_test tests/celboil_test.cpp celnoise.cpp)
    target_link_libraries(celboil_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_boil COMMAND celboil_test)
    add_executable(celboil_ui_test tests/celboil_ui_test.cpp celnoise.cpp)
    target_link_libraries(celboil_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_boil_ui COMMAND celboil_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_celBoilFx.xml)
    add_dependencies(celfx_tests celboil_test celboil_ui_test)
    set_tests_properties(celfx_boil celfx_boil_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

if(BUILD_CELFX_TESTS)
    add_executable(printprocess_test tests/printprocess_test.cpp celnoise.cpp)
    target_link_libraries(printprocess_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_printprocess COMMAND printprocess_test)
    add_executable(printprocess_ui_test tests/printprocess_ui_test.cpp celnoise.cpp)
    target_link_libraries(printprocess_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_printprocess_ui COMMAND printprocess_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_printProcessFx.xml)
    add_dependencies(celfx_tests printprocess_test printprocess_ui_test)
    set_tests_properties(celfx_printprocess celfx_printprocess_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
endif()

if(BUILD_CELFX_TESTS)
    add_executable(celcomposite_test tests/celcomposite_test.cpp)
    target_link_libraries(celcomposite_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_composite COMMAND celcomposite_test)
    add_executable(celcomposite_ui_test tests/celcomposite_ui_test.cpp)
    target_link_libraries(celcomposite_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_composite_ui COMMAND celcomposite_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_lightWrapFx.xml ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_edgeExtendFx.xml ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_matteChokeFx.xml)
    add_dependencies(celfx_tests celcomposite_test celcomposite_ui_test)
    set_tests_properties(celfx_composite celfx_composite_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

if(BUILD_CELFX_TESTS)
    add_executable(flowdisplace_test tests/flowdisplace_test.cpp celnoise.cpp)
    target_link_libraries(flowdisplace_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_flowdisplace COMMAND flowdisplace_test)
    add_executable(flowdisplace_ui_test tests/flowdisplace_ui_test.cpp celnoise.cpp)
    target_link_libraries(flowdisplace_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_flowdisplace_ui COMMAND flowdisplace_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_flowDisplaceFx.xml)
    add_dependencies(celfx_tests flowdisplace_test flowdisplace_ui_test)
    set_tests_properties(celfx_flowdisplace celfx_flowdisplace_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

# Like fxrender, particle renders need a platform plugin with OpenGL support.
# The Linux offscreen plugin uses GLX under the CI runner's Xvfb display.
if(APPLE)
    set(OT_PARTICLE_TEST_QPA_PLATFORM cocoa)
elseif(WIN32)
    set(OT_PARTICLE_TEST_QPA_PLATFORM windows)
else()
    set(OT_PARTICLE_TEST_QPA_PLATFORM offscreen)
endif()

if(BUILD_CELFX_TESTS)
    add_executable(particleweather_test tests/particleweather_test.cpp
        celnoise.cpp)
    target_link_libraries(particleweather_test tnzstdfx Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_particleweather COMMAND particleweather_test)
    add_executable(particleweather_ui_test tests/particleweather_ui_test.cpp)
    target_link_libraries(particleweather_ui_test tnzstdfx toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_particleweather_ui COMMAND particleweather_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_particlesFx.xml)
    add_dependencies(celfx_tests particleweather_test particleweather_ui_test)
    set_tests_properties(celfx_particleweather celfx_particleweather_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${OT_PARTICLE_TEST_QPA_PLATFORM}" TIMEOUT 120)
endif()

if(BUILD_CELFX_TESTS)
    add_executable(alphadistancegradient_test tests/alphadistancegradient_test.cpp)
    target_link_libraries(alphadistancegradient_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_distancegradient COMMAND alphadistancegradient_test)
    add_executable(alphadistancegradient_ui_test tests/alphadistancegradient_ui_test.cpp)
    target_link_libraries(alphadistancegradient_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_distancegradient_ui COMMAND alphadistancegradient_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_alphaDistanceGradientFx.xml)
    add_dependencies(celfx_tests alphadistancegradient_test alphadistancegradient_ui_test)
    set_tests_properties(celfx_distancegradient celfx_distancegradient_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

if(BUILD_CELFX_TESTS)
    add_executable(celsmoother2_test tests/celsmoother2_test.cpp)
    target_link_libraries(celsmoother2_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_smoother2 COMMAND celsmoother2_test)
    add_executable(celsmoother2_ui_test tests/celsmoother2_ui_test.cpp)
    target_link_libraries(celsmoother2_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_smoother2_ui COMMAND celsmoother2_ui_test
             ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_celSmoother2Fx.xml)
    add_dependencies(celfx_tests celsmoother2_test celsmoother2_ui_test)
    set_tests_properties(celfx_smoother2 celfx_smoother2_ui PROPERTIES
                         ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

if(BUILD_CELFX_TESTS)
    add_executable(starglow_test tests/starglow_test.cpp)
    target_link_libraries(starglow_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_starglow COMMAND starglow_test)
    set_tests_properties(celfx_starglow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    add_executable(starglow_ui_test tests/starglow_ui_test.cpp)
    target_link_libraries(starglow_ui_test toonzqt Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME celfx_starglow_ui COMMAND starglow_ui_test ${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff/profiles/layouts/fxs/STD_starGlowFx.xml)
    set_tests_properties(celfx_starglow_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    add_dependencies(celfx_tests starglow_test starglow_ui_test)
endif()

# Native particle cache playback, scene resources and simulation bake regressions.
if(BUILD_PARTICLE_CACHE_TESTS)
    add_executable(particlecache_render_test tests/particlecache_render_test.cpp)
    target_link_libraries(particlecache_render_test tnzstdfx Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME particlecache_render COMMAND particlecache_render_test)
    add_executable(particlecache_bake_test tests/particlecache_bake_test.cpp)
    target_link_libraries(particlecache_bake_test tnzstdfx Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME particlecache_bake COMMAND particlecache_bake_test)
    add_executable(particlecache_bakeisolation_test tests/particlecache_bakeisolation_test.cpp)
    target_link_libraries(particlecache_bakeisolation_test tnzstdfx Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME particlecache_bakeisolation COMMAND particlecache_bakeisolation_test)
    add_executable(particlecache_resources_test tests/particlecache_resources_test.cpp)
    target_link_libraries(particlecache_resources_test tnzstdfx Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    add_test(NAME particlecache_resources COMMAND particlecache_resources_test)
    # Generate the portable example project from the production scene/image I/O.
    add_executable(particlecache_demo tests/particlecache_demo.cpp)
    target_link_libraries(particlecache_demo tnzstdfx image Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui ${TNZLIBS} ${PTHREAD_LIBRARY})
    set_tests_properties(particlecache_render particlecache_bake particlecache_bakeisolation particlecache_resources PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${OT_PARTICLE_TEST_QPA_PLATFORM}" TIMEOUT 300)
    add_custom_target(particlecache_tests DEPENDS particlecache_test particlecache_formats_test particlecache_render_test particlecache_bake_test particlecache_bakeisolation_test particlecache_resources_test particlecache_demo)
endif()
