set(VERSION 1.8)

set(MOC_HEADERS
    languagemanager.h
    commandpalette.h
    widgetinspector.h
    themeapplication.h
    nativediagnostics.h
    diagnosticssettingswidget.h
    cadmiumruntime.h
    cadmiumpopup.h
    cadmiumsettingswidget.h
    cadmiuminstaller.h
    cadmiumreviewview.h
    addfilmstripframespopup.h
    adjustlevelspopup.h
    adjustthicknesspopup.h
    antialiaspopup.h
    audiorecordingpopup.h
    autoinputcellnumberpopup.h
    autosaverecovery.h
    batches.h
    batchserversviewer.h
    binarizepopup.h
    fillholespopup.h
    boardsettingspopup.h
    brightnessandcontrastpopup.h
    cachefxcommand.h
    camerasettingspopup.h
    canvassizepopup.h
    castviewer.h
    cleanuppaletteviewer.h
    cleanuppopup.h
    cleanuppreview.h
    cleanupsettingsmodel.h
    cleanupsettingspane.h
    cleanupsettingspopup.h
    cleanupswatch.h
    colormodelbehaviorpopup.h
    colormodelviewer.h
    comboviewerpane.h
    commandbar.h
    commandbarpopup.h
    convertpopup.h
    crashhandler.h
    duplicatepopup.h
    dvdirtreeview.h
    dvitemview.h
    dvwidgets.h
    exportlevelpopup.h
    exportalllevelspopup.h
    exportpanel.h
    exportscenepopup.h
    filebrowser.h
    filebrowsermodel.h
    filebrowserpopup.h
    filmstrip.h
    flipbook.h
    formatsettingspopups.h
    frameheadgadget.h
    fxparameditorpopup.h
    histogrampopup.h
    historypane.h
    imageviewer.h
    layerfooterpanel.h
    layerheaderpanel.h
    levelcreatepopup.h
    levelsettingspopup.h
    linesfadepopup.h
    lipsyncpopup.h
    autolipsyncpopup.h
    loadfolderpopup.h
    locatorpopup.h
    magpiefileimportpopup.h
    mainwindow.h
    matchline.h
    menubar.h
    menubarpopup.h
    meshifypopup.h
    messagepanel.h
    ocaio.h
    onionskinmaskgui.h
    outputsettingspopup.h
    overwritepopup.h
    pane.h
    pltgizmopopup.h
    preferencespopup.h
    previewer.h
    previewfxmanager.h
    projectpopup.h
    psdsettingspopup.h
    reframepopup.h
    renumberpopup.h
    reslist.h
    ruler.h
    savepresetpopup.h
    scanpopup.h
    scenebrowser.h
    scenesettingspopup.h
    scenecheckpane.h
    sceneviewer.h
    sceneviewercontextmenu.h
    scriptconsolepanel.h
    brushpresetpanel.h
    toolpropertiespanel.h
    separatecolorspopup.h
    separatecolorsswatch.h
    shortcutpopup.h
    soundtrackexport.h
    startuppopup.h
    styleshortcutswitchablepanel.h
    svncleanupdialog.h
    svncommitdialog.h
    svndeletedialog.h
    svnlockdialog.h
    svnlockframerangedialog.h
    svnpurgedialog.h
    svnrevertdialog.h
    svnupdateandlockdialog.h
    svnupdatedialog.h
    tapp.h
    tasksviewer.h
    testpanel.h
    timestretchpopup.h
    toolbar.h
    tpanels.h
    trackerpopup.h
    alignmentpane.h
    posespane.h
    referencepanel.h
    purerefboard.h
    drawingsubstitutionpane.h
    substitutionsliderpane.h
    storyboardpane.h
    mastercontrollerspane.h
    offsetlayerspane.h
    artlayermanagerpane.h
    spacingchartpane.h
    motionpresetspane.h
    riglibrarypane.h
    symbolspane.h
    vectorguideddrawingpane.h
    vectorizerpopup.h
    vectorizerswatch.h
    versioncontrol.h
    versioncontrolxmlwriter.h
    versioncontroltimeline.h
    versioncontrolwidget.h
    viewerpane.h
    xshcellviewer.h
    xshcolumnviewer.h
    xsheetviewer.h
    xshnoteviewer.h
    xshrowviewer.h
    xshtoolbar.h
    xdtsimportpopup.h
    expressionreferencemanager.h
    tooloptionsshortcutinvoker.h
    exportxsheetpdf.h
    custompanelmanager.h
    toolpresetcommandmanager.h
    custompaneleditorpopup.h
    convertfolderpopup.h
    exportcameratrackpopup.h
    exportlayeredpsdpopup.h
)

if(PLATFORM EQUAL 64)
    set(MOC_HEADERS
        ${MOC_HEADERS}
        ../stopmotion/stopmotion.h
        ../stopmotion/stopmotioncontroller.h
        ../stopmotion/webcam.h
        ../stopmotion/jpgconverter.h
        ../stopmotion/canon.h
        ../stopmotion/stopmotionserial.h
        ../stopmotion/stopmotionlight.h
        cameracapturelevelcontrol.h
        penciltestpopup.h
        navtageditorpopup.h
        xshbreadcrumbs.h
    )
else()
    set(MOC_HEADERS
        ${MOC_HEADERS}
        cameracapturelevelcontrol_qt.h
        penciltestpopup_qt.h
        navtageditorpopup.h
        xshbreadcrumbs.h
    )
endif()

if(WITH_PYTHON)
    set(MOC_HEADERS ${MOC_HEADERS} pythonconsolepanel.h)
endif()

set(HEADERS
    cadmiumtypes.h
    cadmiumsource.h
    cadmiumcolor.h
    cadmiumconsensus.h
    cadmiumregeneration.h
    cadmiumsession.h
    cadmiumdestination.h
    ${MOC_HEADERS}
    celldata.h
    cellkeyframedata.h
    cellkeyframeselection.h
    cellselection.h
    columncommand.h
    columncoloreffectpopup.h
    columnselection.h
    customhelplink.h
    curveio.h
    drawingdata.h
    drawingsubstitutionundo.h
    exportlevelcommand.h
    filedata.h
    fileselection.h
    filmstripcommand.h
    filmstripselection.h
    floatingpanelcommand.h
    history.h
    iocommand.h
    keyframedata.h
    keyframemover.h
    keyframeselection.h
    kis_tablet_support_win8.h
    menubarcommandids.h
    moviegenerator.h
    scanlist.h
    sceneviewerevents.h
    selectionutils.h
    subcameramanager.h
    subscenecommand.h
    symbolcommand.h
    distributecommand.h
    distributeutil.h
    librarycommand.h
    tfarmstuff.h
    versioncontrolgui.h
    versioncontrolxmlreader.h
    viewerdraw.h
    aspectguides.h
    pegbarguides.h
    motionarcs.h
    viewerpopup.h
    xshcellmover.h
    xsheetdragtool.h
    castselection.h
    ../include/cellposition.h
    ../include/orientation.h
    ../include/saveloadqsettings.h
    xdtsio.h
    sxfio.h
    levelcommand.h
    spineexport.h
    textureatlasexport.h
# Tracker file
    ObjectTracker.h
    dummyprocessor.h
    metnum.h
    processor.h
    predict3d.h
    tvpjson_io.h
)

set(SOURCES
    languagemanager.cpp
    commandpalette.cpp
    commandpalettenative.cpp
    widgetinspector.cpp
    themeapplication.cpp
    nativethemeapplication.cpp
    nativediagnosticswriter.cpp
    nativediagnostics.cpp
    diagnosticssettingswidget.cpp
    cadmiumruntime.cpp
    cadmiumdestination.cpp
    cadmiumpopup.cpp
    cadmiumpopupui.cpp
    cadmiumreviewview.cpp
    cadmiumconsensus.cpp
    cadmiumregeneration.cpp
    cadmiumsession.cpp
    cadmiumsettingswidget.cpp
    cadmiuminstaller.cpp
    cadmiumtoonzraster.cpp
    cadmiumvector.cpp
    cadmiumfullcolor.cpp
    floatingpanelcommand.cpp
    canvassizepopup.cpp
    commandbar.cpp
    commandbarpopup.cpp
    history.cpp
    loadfoldercommand.cpp
    loadfolderpopup.cpp
    main.cpp
    mainwindow.cpp
    customhelplink.cpp
    matchline.cpp
    menubar.cpp
    menubarpopup.cpp
    mergecolumns.cpp
    moviegenerator.cpp
    pane.cpp
    previewer.cpp
    previewfxmanager.cpp
    ruler.cpp
    scanlist.cpp
    tapp.cpp
    toolbar.cpp
    batchserversviewer.cpp
    castviewer.cpp
    exportlevelpopup.cpp
    exportalllevelspopup.cpp
    exportscenepopup.cpp
    colormodelviewer.cpp
    exportpanel.cpp
    filebrowser.cpp
    filebrowserversioncontrol.cpp
    filmstrip.cpp
    matchlinecommand.cpp
    messagepanel.cpp
    tasksviewer.cpp
    testpanel.cpp
    tpanels.cpp
    xshcellmover.cpp
    xshcellviewer.cpp
    xshcolumnviewer.cpp
    columncoloreffectpopup.cpp
    xsheetdragtool.cpp
    xsheetviewer.cpp
    xshnoteviewer.cpp
    xshrowviewer.cpp
    xshtoolbar.cpp
    sceneviewer.cpp
    sceneviewerevents.cpp
    viewerdraw.cpp
    aspectguides.cpp
    pegbarguides.cpp
    motionarcs.cpp
    viewerpane.cpp
    castselection.cpp
    cellselection.cpp
    columnselection.cpp
    fileselection.cpp
    filmstripselection.cpp
    keyframeselection.cpp
    addfilmstripframespopup.cpp
    camerasettingspopup.cpp
    convertpopup.cpp
    convertlinestoareascommand.cpp
    rigeasecommand.cpp
    rigmappingcommand.cpp
    crashhandler.cpp
    autosaverecovery.cpp
    duplicatepopup.cpp
    dvdirtreeview.cpp
    filebrowserpopup.cpp
    formatsettingspopups.cpp
    fxparameditorpopup.cpp
    histogrampopup.cpp
    layerfooterpanel.cpp
    layerheaderpanel.cpp
    levelcreatepopup.cpp
    levelsettingspopup.cpp
    lipsyncpopup.cpp
    autolipsyncpopup.cpp
    magpiefileimportpopup.cpp
    outputsettingspopup.cpp
    overwritepopup.cpp
    pltgizmopopup.cpp
    preferencespopup.cpp
    projectpopup.cpp
    psdsettingspopup.cpp
    renumberpopup.cpp
    runscriptcommand.cpp
    savepresetpopup.cpp
    scanpopup.cpp
    scenebrowser.cpp
    scenebrowserversioncontrol.cpp
    sceneviewercontextmenu.cpp
    scenesettingspopup.cpp
    scenecheckpane.cpp
    scriptconsolepanel.cpp
    brushpresetpanel.cpp
    toolpropertiespanel.cpp
    shortcutpopup.cpp
    soundtrackexport.cpp
    startuppopup.cpp
    subcameramanager.cpp
    kis_tablet_support_win8.cpp
    timestretchpopup.cpp
    trackerpopup.cpp
    alignmentpane.cpp
    posespane.cpp
    referencepanel.cpp
    purerefboard.cpp
    drawingsubstitutionpane.cpp
    drawingsubstitutionundo.cpp
    substitutionsliderpane.cpp
    storyboardpane.cpp
    mastercontrollerspane.cpp
    offsetlayerspane.cpp
    artlayermanagerpane.cpp
    spacingchartpane.cpp
    createcolourartcommand.cpp
    motionpresetspane.cpp
    riglibrarypane.cpp
    symbolspane.cpp
    vectorguideddrawingpane.cpp
    vectorizerpopup.cpp
    rasterizecommand.cpp
    vectorizerswatch.cpp
    versioncontrol.cpp
    versioncontroltimeline.cpp
    versioncontrolxmlreader.cpp
    versioncontrolxmlwriter.cpp
    versioncontrolwidget.cpp
    viewerpopup.cpp
    cellselectioncommand.cpp
    columncommand.cpp
    tonehighlightcommand.cpp
    filmstripcommand.cpp
    iocommand.cpp
    levelcommand.cpp
    viewercommand.cpp
    print.cpp
    subscenecommand.cpp
    symbolcommand.cpp
    distributecommand.cpp
    librarycommand.cpp
    svncleanupdialog.cpp
    svncommitdialog.cpp
    svndeletedialog.cpp
    svnlockdialog.cpp
    svnlockframerangedialog.cpp
    svnrevertdialog.cpp
    svnupdateandlockdialog.cpp
    svnupdatedialog.cpp
    svnpurgedialog.cpp
    vcrcommand.cpp
    xsheetcmd.cpp
    celldata.cpp
    drawingdata.cpp
    filedata.cpp
    imageviewer.cpp
    keyframedata.cpp
    dvitemview.cpp
    dvwidgets.cpp
    flipbook.cpp
    frameheadgadget.cpp
    onionskinmaskgui.cpp
    batches.cpp
    tfarmstuff.cpp
    cleanuppaletteviewer.cpp
    cleanuppopup.cpp
    cleanuppreview.cpp
    cleanupsettingsmodel.cpp
    cleanupsettingspopup.cpp
    cleanupswatch.cpp
    cellkeyframedata.cpp
    cellkeyframeselection.cpp
    keyframemover.cpp
    cachefxcommand.cpp
    exportlevelcommand.cpp
    adjustlevelspopup.cpp
    adjustthicknesspopup.cpp
    binarizepopup.cpp
    fillholespopup.cpp
    brightnessandcontrastpopup.cpp
    linesfadepopup.cpp
    rendercommand.cpp
    curveio.cpp
    meshifypopup.cpp
    selectionutils.cpp
    antialiaspopup.cpp
    mergecmapped.cpp
    comboviewerpane.cpp
    historypane.cpp
    cleanupsettingspane.cpp
    audiorecordingpopup.cpp
    locatorpopup.cpp
    styleshortcutswitchablepanel.cpp
    reframepopup.cpp
    autoinputcellnumberpopup.cpp
    colormodelbehaviorpopup.cpp
    boardsettingspopup.cpp
    separatecolorsswatch.cpp
    separatecolorspopup.cpp
    xdtsio.cpp
    sxfio.cpp
    ocaio.cpp
    exportpdf.cpp
    importpdf.cpp
    importpdf.h
    animatedsvgexport.cpp
    lottieexport.cpp
    xdtsimportpopup.cpp
    expressionreferencemanager.cpp
    tooloptionsshortcutinvoker.cpp
    tvpjson_io.cpp
    exportxsheetpdf.cpp
    custompanelmanager.cpp
    toolpresetcommandmanager.cpp
    custompaneleditorpopup.cpp
    convertfolderpopup.cpp
    exportcameratrackpopup.cpp
    exportlayeredpsdpopup.cpp
    spineexport.cpp
    textureatlasexport.cpp
    abrimport.cpp
    deformerbake.cpp
    particlecachepopup.cpp
# Tracker file
    dummyprocessor.cpp
    metnum.cpp
    ObjectTracker.cpp
    predict3d.cpp
)

if(PLATFORM EQUAL 64)
    set(SOURCES
        ${SOURCES}
        ../stopmotion/stopmotion.cpp
        ../stopmotion/stopmotioncontroller.cpp
        ../stopmotion/webcam.cpp
        ../stopmotion/jpgconverter.cpp
        ../stopmotion/canon.cpp
        ../stopmotion/stopmotionserial.cpp
        ../stopmotion/stopmotionlight.cpp
        cameracapturelevelcontrol.cpp
        penciltestpopup.cpp
        navtageditorpopup.cpp
        xshbreadcrumbs.cpp
    )
else()
    set(SOURCES
        ${SOURCES}
        cameracapturelevelcontrol_qt.cpp
        penciltestpopup_qt.cpp
        navtageditorpopup.cpp
        xshbreadcrumbs.cpp
    )
endif()

if(WITH_PYTHON)
    set(SOURCES ${SOURCES} pythonconsolepanel.cpp)
endif()


set(OBJCSOURCES filebrowsermodel.cpp)

# Resource file(s)
set(RESOURCES toonz.qrc)

if(BUILD_ENV_MSVC)
    set(RC_FILE toonz.rc)
endif()

ot_qt_add_resources(SOURCES ${RESOURCES})

if(BUILD_ENV_APPLE)
    #set_source_files_properties(${OBJCSOURCES} PROPERTIES LANGUAGE OBJCXX)
    set_source_files_properties(${OBJCSOURCES} PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()

ot_qt_wrap_cpp(SOURCES ${MOC_HEADERS})

if(BUILD_ENV_MSVC)
    add_executable(OpenToonz WIN32 ${HEADERS} ${SOURCES} ${OBJCSOURCES} ${RESOURCES} ${RC_FILE})
else()
    set(MACOSX_BUNDLE_ICON_FILE "OpenToonz.icns")
    set_source_files_properties(OpenToonz.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    add_executable(OpenToonz MACOSX_BUNDLE OpenToonz.icns ${HEADERS} ${SOURCES} ${OBJCSOURCES} ${RESOURCES})
    add_dependencies(OpenToonz tnzcore tnzbase toonzlib colorfx tnzext image sound toonzqt tnztools tnzstdfx tfarm)
    #add_definitions(
    #    -DTNZBASE_EXPORTS
    #    -DTPARAM_EXPORTS
    #    -DTXSHEET_EXPORTS
    #    -DTFX_EXPORTS
    #    -DTAPPTOOLS_EXPORTS
    #    -DTTEST_EXPORTS
    #)
endif()

if(WITH_TRANSLATION)
    add_translation(toonz ${HEADERS} ${SOURCES})
    add_compile_translations_command(OpenToonz toonz)
endif()

if(BUILD_TARGET_WIN)
    add_definitions(
        -DUNICODE
        -D_UNICODE
    )
endif()

message("subdir: toonz")
message("Bin: " ${CMAKE_CURRENT_BINARY_DIR})
#message("Lib: " ${LIBRARY_OUTPUT_PATH})
#message("Exe: " ${EXECUTABLE_OUTPUT_PATH})
#message("Sources:" ${SOURCES})

include_directories(
    ../../sources/toonzfarm/include
    ../../sources/toonzqt
    ../../sources/stopmotion
    ../../sources/tnztools   # Required for SelectionScaleField, SelectionRotationField, etc.
)

if (WITH_CANON)
    add_definitions(-DWITH_CANON)
endif()

if (WITH_PYTHON)
    add_definitions(-DWITH_PYTHON)
endif()

# The WinTab switch exists in the customized Qt 5.15.2 only (not ported to Qt 6)
if (WITH_WINTAB AND BUILD_TARGET_WIN AND (PLATFORM EQUAL 64) AND (QT_VERSION_MAJOR EQUAL 5))
    add_definitions(-DWITH_WINTAB)
endif()

if(BUILD_ENV_APPLE)
    include_directories(../../sources/mousedragfilter)
    if(PLATFORM EQUAL 64)
        include_directories(${TURBOJPEG_INCLUDE_DIRS})
    endif()
endif()

if(BUILD_TARGET_WIN)
    if(PLATFORM EQUAL 32)
        include_directories(
            SYSTEM
            ${SDKROOT}/quicktime/QT73SDK/CIncludes
        )
    endif()
endif()

if(BUILD_ENV_MSVC)
    set(EXTRA_LIBS)
    if(CMAKE_CXX_FLAGS_DEBUG)
        target_compile_options(OpenToonz PRIVATE /EHsc) # Enable C++ Exceptions in Debug Mode.
    endif()
    if(PLATFORM EQUAL 64)
        set(EXTRA_LIBS ${EXTRA_LIBS} Qt${QT_VERSION_MAJOR}::SerialPort ${TURBOJPEG_LIB} ${OpenCV_LIBS})
        if(WITH_CANON)
            set(EXTRA_LIBS ${EXTRA_LIBS} ${CANON_LIB})
        endif()
    endif()
    target_link_libraries(OpenToonz
        ${OT_QT_WINMAIN_LIB} Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::OpenGL ${OT_QT_OPENGLWIDGETS_LIB} Qt${QT_VERSION_MAJOR}::Svg Qt${QT_VERSION_MAJOR}::Xml
        Qt${QT_VERSION_MAJOR}::Qml Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::PrintSupport Qt${QT_VERSION_MAJOR}::Multimedia Qt${QT_VERSION_MAJOR}::UiTools ${OT_QT_CORE5COMPAT_LIB}
        ${GL_LIB} ${GLUT_LIB}
        ${EXTRA_LIBS} strmiids
        tnzcore tnzbase toonzlib colorfx tnzext image sound toonzqt tnztools tnzstdfx tfarm
    )
elseif(BUILD_ENV_APPLE)
    find_library(COCOA_LIB Cocoa)

    _find_toonz_library(EXTRA_LIBS "tnzcore;tnzbase;toonzlib;colorfx;tnzext;image;sound;toonzqt;tnztools")

    # Workaround for non-standard library output locations
    set(EXTRA_LIBS ${EXTRA_LIBS} "$<TARGET_FILE:tnzstdfx>" "$<TARGET_FILE:tfarm>")
    set(X64ONLY_LIBS)
    if(PLATFORM EQUAL 64)
        set(X64ONLY_LIBS ${X64ONLY_LIBS} Qt${QT_VERSION_MAJOR}::SerialPort ${TURBOJPEG_LIB} ${OpenCV_LIBS})
        if(WITH_CANON)
            set(X64ONLY_LIBS ${X64ONLY_LIBS} ${CANON_LIB})
        endif()
    endif()

    target_link_directories(OpenToonz PRIVATE ${MYPAINT_LIB_LIBRARY_DIRS})

    target_link_libraries(OpenToonz
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::OpenGL ${OT_QT_OPENGLWIDGETS_LIB} Qt${QT_VERSION_MAJOR}::Svg Qt${QT_VERSION_MAJOR}::Xml
        Qt${QT_VERSION_MAJOR}::Qml Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::PrintSupport Qt${QT_VERSION_MAJOR}::Multimedia Qt${QT_VERSION_MAJOR}::MultimediaWidgets Qt${QT_VERSION_MAJOR}::UiTools ${OT_QT_CORE5COMPAT_LIB}
        ${GL_LIB} ${GLUT_LIB}
        ${COCOA_LIB} ${EXTRA_LIBS} ${X64ONLY_LIBS} mousedragfilter
        ${MYPAINT_LIB_LIBRARIES}
    )

    # ADDED/UPDATED: Expanded dependencies for all libs and utilities (ensures build order)
    add_dependencies(OpenToonz tnzcore tnzbase toonzlib colorfx tnzext image sound toonzqt tnztools tnzstdfx tfarm tcomposer tcleanup tconverter tfarmcontroller tfarmserver)

    # ADDED/UPDATED: Configure Info.plist for bundle (retina support etc.)
    set_target_properties(OpenToonz PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/../cmake/BundleInfo.plist.in)

    # ADDED/UPDATED: Merged RPATH command (single one with both paths; || true for CI; COMMENT for logs)
    add_custom_command(TARGET OpenToonz POST_BUILD
        COMMAND ${CMAKE_INSTALL_NAME_TOOL} -add_rpath @executable_path/. "$<TARGET_FILE:OpenToonz>" || true
        COMMAND ${CMAKE_INSTALL_NAME_TOOL} -add_rpath ${CMAKE_INSTALL_RPATH} "$<TARGET_FILE:OpenToonz>" || true
        COMMENT "Adding RPATHs to OpenToonz executable"
    )

elseif(BUILD_ENV_UNIXLIKE)
    _find_toonz_library(EXTRA_LIBS "tnzcore;tnzbase;toonzlib;colorfx;tnzext;image;sound;toonzqt;tnztools")

    # Workaround for non-standard library output locations
    set(EXTRA_LIBS ${EXTRA_LIBS} "$<TARGET_FILE:tnzstdfx>" "$<TARGET_FILE:tfarm>")

    set(EXTRA_LIBS ${EXTRA_LIBS} ${Boost_LIBRARIES} ${OPENBLAS_LIB} ${EXECINFO_LIBRARY})
    if(PLATFORM EQUAL 64)
        set(EXTRA_LIBS ${EXTRA_LIBS} Qt${QT_VERSION_MAJOR}::SerialPort ${TURBOJPEG_LIB} ${OpenCV_LIBS})
    endif()

    if(BUILD_TARGET_WIN)
        set(EXTRA_LIBS ${EXTRA_LIBS} ${OT_QT_WINMAIN_LIB} -lstrmiids -mwindows)
    endif()

    target_link_libraries(
        OpenToonz Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::OpenGL ${OT_QT_OPENGLWIDGETS_LIB} Qt${QT_VERSION_MAJOR}::Svg Qt${QT_VERSION_MAJOR}::Xml
        Qt${QT_VERSION_MAJOR}::Qml Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::PrintSupport Qt${QT_VERSION_MAJOR}::Multimedia Qt${QT_VERSION_MAJOR}::UiTools ${OT_QT_CORE5COMPAT_LIB}
        ${GL_LIB} ${GLUT_LIB} ${GLU_LIB}
        ${EXTRA_LIBS}
    )
endif()

if(BUILD_ENV_APPLE)
    # CMAKE_RUNTIME_OUTPUT_DIRECTORY should be equivalent to usage on windows despite empty
    # On OSX, CMAKE_RUNTIME_OUTPUT_DIRECTORY is empty; ensuring it can be used the same way as the Windows version.
    # Removed: get_target_property(bin OpenToonz LOCATION)
    # Use generator expression for runtime directory
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:OpenToonz>")
    message("CMAKE_RUNTIME_OUTPUT_DIRECTORY:" ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()

# copy utility executables onto the directory after build
if(LZODRIVER_FOUND)
    add_custom_command(TARGET OpenToonz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:lzocompress> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMENT "Copying lzocompress to output dir"
    )
    add_custom_command(TARGET OpenToonz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:lzodecompress> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMENT "Copying lzodecompress to output dir"
    )
    add_dependencies(OpenToonz lzocompress lzodecompress)  # Ensures order
endif()

if(BUILD_ENV_APPLE)
    get_target_property(loc OpenToonz MACOSX_BUNDLE_NAME)
    message(" ==> App Bundle: " ${loc})
    message(" ==> Mach-o: " $<TARGET_FILE:OpenToonz>)  # Fixed: Use generator expression instead of ${bin}
    foreach(lib ${EXTRA_LIBS})
        message(" copy:" ${lib} "==>" ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
        add_custom_command(TARGET OpenToonz POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${lib} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}  # UPDATED: Use CMAKE_COMMAND for portability (instead of cp)
            COMMENT "Copying lib ${lib} to output dir"
        )
    endforeach()
    # ADDED: Already included in expanded dependencies above

    add_custom_command(TARGET OpenToonz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tcomposer> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMENT "Copying tcomposer to output dir"
    )
    add_custom_command(TARGET OpenToonz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tcleanup> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMENT "Copying tcleanup to output dir"
    )
    add_custom_command(TARGET OpenToonz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tconverter> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMENT "Copying tconverter to output dir"
    )
    add_custom_command(TARGET OpenToonz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tfarmcontroller> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMENT "Copying tfarmcontroller to output dir"
    )
    add_custom_command(TARGET OpenToonz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tfarmserver> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMENT "Copying tfarmserver to output dir"
    )
    # ADDED: Already included in expanded dependencies above

    add_custom_command(TARGET OpenToonz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/../Resources
        COMMENT "Creating Resources directory"
    )
    add_custom_command(TARGET OpenToonz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/../install/SystemVar.ini ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/../Resources
        COMMENT "Copying SystemVar.ini to Resources"
    )

    add_custom_command(TARGET OpenToonz POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/../../../qss
        COMMENT "Creating qss directory"
    )

    # REMOVED: The duplicate RPATH commands were merged at the top of if(BUILD_ENV_APPLE)
endif()

if(BUILD_ENV_UNIXLIKE AND BUILD_TARGET_WIN)
    # Setup files on Windows platform while cross compilation.
    install(
        TARGETS
            OpenToonz
        DESTINATION bin
    )

    install(
        PROGRAMS
            "$<TARGET_FILE:tcleanup>"
            "$<TARGET_FILE:tcomposer>"
            "$<TARGET_FILE:tconverter>"
            "$<TARGET_FILE:tfarmcontroller>"
            "$<TARGET_FILE:tfarmserver>"
        DESTINATION bin
    )

    if(LZODRIVER_FOUND)
        install(
            PROGRAMS
                "$<TARGET_FILE:lzocompress>"
                "$<TARGET_FILE:lzodecompress>"
            DESTINATION bin
        )
    endif()

    install(
        FILES
            "$<TARGET_FILE:tnzcore>"
            "$<TARGET_FILE:tnzbase>"
            "$<TARGET_FILE:toonzlib>"
            "$<TARGET_FILE:colorfx>"
            "$<TARGET_FILE:tnzext>"
            "$<TARGET_FILE:image>"
            "$<TARGET_FILE:sound>"
            "$<TARGET_FILE:toonzqt>"
            "$<TARGET_FILE:tnztools>"
            "$<TARGET_FILE:tnzstdfx>"
            "$<TARGET_FILE:tfarm>"
        DESTINATION bin
    )

    install(
        DIRECTORY
            ${CMAKE_SOURCE_DIR}/../../stuff
        DESTINATION share/opentoonz
    )
elseif(BUILD_ENV_UNIXLIKE AND NOT BUILD_TARGET_WIN)
    # Setup files on Linux/Unix platform.
    # not essential since the binary can run in-place,
    # but useful for package managers.
    #
    # Note that its assumed the destination can be either
    # - /opt/opentoonz
    # - /usr
    install(
        TARGETS
            OpenToonz
        DESTINATION bin
    )

    install(
        PROGRAMS
            "$<TARGET_FILE:tcleanup>"
            "$<TARGET_FILE:tcomposer>"
            "$<TARGET_FILE:tconverter>"
            "$<TARGET_FILE:tfarmcontroller>"
            "$<TARGET_FILE:tfarmserver>"
        DESTINATION bin
    )

    # lowercase symlink so the .desktop file's Exec=opentoonz keeps working
    install(CODE "
      file(REMOVE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/opentoonz\")
      execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink
                      OpenToonz
                      \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/opentoonz\")
    ")

    if(LZODRIVER_FOUND)
        install(
            PROGRAMS
                "$<TARGET_FILE:lzocompress>"
                "$<TARGET_FILE:lzodecompress>"
            DESTINATION bin
        )
    endif()

    install(
        FILES
            "$<TARGET_FILE:tnzcore>"
            "$<TARGET_FILE:tnzbase>"
            "$<TARGET_FILE:toonzlib>"
            "$<TARGET_FILE:colorfx>"
            "$<TARGET_FILE:tnzext>"
            "$<TARGET_FILE:image>"
            "$<TARGET_FILE:sound>"
            "$<TARGET_FILE:toonzqt>"
            "$<TARGET_FILE:tnztools>"
            "$<TARGET_FILE:tnzstdfx>"
            "$<TARGET_FILE:tfarm>"
        DESTINATION lib/opentoonz
    )

    install(
        DIRECTORY
            ${CMAKE_SOURCE_DIR}/../../stuff
        DESTINATION share/opentoonz
    )

endif()

# Optional regression target for the Spine export math (spineexport.h,
# header-only helpers).
option(BUILD_SPINE_EXPORT_TESTS "Build the Spine export math test" ${BUILD_TESTS})
if(BUILD_SPINE_EXPORT_TESTS)
    add_executable(spineexport_test tests/spineexport_test.cpp)
    target_include_directories(spineexport_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME spineexport COMMAND spineexport_test)
endif()

option(BUILD_PUREREF_TESTS "Build the PureRef board import regression test" ${BUILD_TESTS})
if(BUILD_PUREREF_TESTS)
    add_executable(purerefboard_test tests/purerefboard_test.cpp purerefboard.cpp)
    target_include_directories(purerefboard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(purerefboard_test PRIVATE Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
    add_test(NAME purerefboard COMMAND purerefboard_test)
    set_tests_properties(purerefboard PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()

# Optional regression target for the helpers of Distribute to Layers / Frames
# (distributeutil.h, header-only).
option(BUILD_DISTRIBUTE_TESTS "Build the Distribute to Layers/Frames helpers test" ${BUILD_TESTS})
if(BUILD_DISTRIBUTE_TESTS)
    add_executable(distributeutil_test tests/distributeutil_test.cpp)
    target_include_directories(distributeutil_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME distributeutil COMMAND distributeutil_test)
endif()

option(BUILD_CADMIUM_TESTS "Build Cadmium native adapter and workflow regression tests" ${BUILD_TESTS})
if(BUILD_CADMIUM_TESTS)
    foreach(adapter toonzraster fullcolor vector)
        add_executable(cadmium_${adapter}_test
            tests/cadmium${adapter}_test.cpp cadmium${adapter}.cpp)
        target_include_directories(cadmium_${adapter}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(cadmium_${adapter}_test PRIVATE
            Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets
            Qt${QT_VERSION_MAJOR}::OpenGL ${OT_QT_OPENGLWIDGETS_LIB}
            tnzcore tnzbase tnzext toonzlib ${GL_LIB} ${GLUT_LIB})
        add_test(NAME cadmium_${adapter} COMMAND cadmium_${adapter}_test)
    endforeach()
    # The vector test needs an OpenGL context and skips without one
    set_tests_properties(cadmium_vector PROPERTIES SKIP_RETURN_CODE 77)
    add_custom_target(cadmium_tests DEPENDS cadmium_toonzraster_test cadmium_fullcolor_test cadmium_vector_test)
endif()

if(BUILD_CADMIUM_TESTS)
    add_executable(cadmium_codec_test tests/cadmiumcodec_test.cpp)
    target_include_directories(cadmium_codec_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cadmium_codec_test PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui tnzcore)
    add_test(NAME cadmium_codec COMMAND cadmium_codec_test)
    add_executable(cadmium_settings_test tests/cadmiumsettings_test.cpp
        cadmiumruntime.cpp cadmiumsettingswidget.cpp cadmiuminstaller.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_cadmiumruntime.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_cadmiumsettingswidget.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_cadmiuminstaller.cpp)
    target_include_directories(cadmium_settings_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cadmium_settings_test PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
    add_test(NAME cadmium_settings COMMAND cadmium_settings_test)
    add_dependencies(cadmium_tests cadmium_codec_test cadmium_settings_test)
endif()

if(BUILD_CADMIUM_TESTS)
    add_executable(cadmium_pipeline_test tests/cadmiumpipeline_test.cpp
        cadmiumruntime.cpp cadmiumtoonzraster.cpp cadmiumvector.cpp cadmiumfullcolor.cpp cadmiumconsensus.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_cadmiumruntime.cpp)
    target_include_directories(cadmium_pipeline_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cadmium_pipeline_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::OpenGL ${OT_QT_OPENGLWIDGETS_LIB}
        tnzcore tnzbase tnzext toonzlib ${GL_LIB} ${GLUT_LIB})
    add_test(NAME cadmium_pipeline COMMAND cadmium_pipeline_test)
    set_tests_properties(cadmium_pipeline PROPERTIES SKIP_RETURN_CODE 77)
    add_dependencies(cadmium_tests cadmium_pipeline_test)
endif()

if(BUILD_CADMIUM_TESTS)
    add_executable(cadmium_metadata_test tests/cadmiummetadata_test.cpp)
    target_include_directories(cadmium_metadata_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cadmium_metadata_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui
        tnzcore tnzbase tnzext toonzlib image)
    add_test(NAME cadmium_metadata COMMAND cadmium_metadata_test)
    add_dependencies(cadmium_tests cadmium_metadata_test)
endif()

if(BUILD_CADMIUM_TESTS)
    add_executable(cadmium_installer_test tests/cadmiuminstaller_test.cpp cadmiuminstaller.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_cadmiuminstaller.cpp)
    target_include_directories(cadmium_installer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cadmium_installer_test PRIVATE Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)
    add_test(NAME cadmium_installer COMMAND cadmium_installer_test)
    set_tests_properties(cadmium_installer PROPERTIES SKIP_RETURN_CODE 77)
    add_dependencies(cadmium_tests cadmium_installer_test)
endif()

if(BUILD_CADMIUM_TESTS)
    add_executable(cadmium_save_test tests/cadmiumsave_test.cpp)
    target_include_directories(cadmium_save_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cadmium_save_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui
        tnzcore tnzbase tnzext toonzlib image OpenEXR::OpenEXR)
    add_test(NAME cadmium_save COMMAND cadmium_save_test)
    add_dependencies(cadmium_tests cadmium_save_test)
endif()

if(BUILD_CADMIUM_TESTS)
    add_executable(cadmium_destination_test tests/cadmiumdestination_test.cpp cadmiumdestination.cpp)
    target_include_directories(cadmium_destination_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cadmium_destination_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui
        tnzcore tnzbase tnzext toonzlib image)
    add_test(NAME cadmium_destination COMMAND cadmium_destination_test)
    add_dependencies(cadmium_tests cadmium_destination_test)
endif()

if(BUILD_CADMIUM_TESTS)
    add_executable(cadmium_color_test tests/cadmiumcolor_test.cpp)
    target_include_directories(cadmium_color_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cadmium_color_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui tnzcore)
    add_test(NAME cadmium_color COMMAND cadmium_color_test)
    add_dependencies(cadmium_tests cadmium_color_test)
endif()

if(BUILD_CADMIUM_TESTS)
    foreach(workflow consensus regeneration session workflow)
        set(cadmium_workflow_sources cadmiumconsensus.cpp)
        if(NOT workflow STREQUAL "consensus")
            list(APPEND cadmium_workflow_sources cadmiumregeneration.cpp)
        endif()
        if(workflow STREQUAL "session" OR workflow STREQUAL "workflow")
            list(APPEND cadmium_workflow_sources cadmiumsession.cpp)
        endif()
        add_executable(cadmium_${workflow}_test tests/cadmium${workflow}_test.cpp ${cadmium_workflow_sources})
        target_include_directories(cadmium_${workflow}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(cadmium_${workflow}_test PRIVATE
            Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui
            tnzcore tnzbase tnzext toonzlib image)
        add_test(NAME cadmium_${workflow} COMMAND cadmium_${workflow}_test)
        add_dependencies(cadmium_tests cadmium_${workflow}_test)
        set_tests_properties(cadmium_${workflow} PROPERTIES TIMEOUT 120)
    endforeach()
    add_executable(cadmium_reviewview_test tests/cadmiumreviewview_test.cpp cadmiumreviewview.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_cadmiumreviewview.cpp)
    target_include_directories(cadmium_reviewview_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cadmium_reviewview_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
    add_test(NAME cadmium_reviewview COMMAND cadmium_reviewview_test)
    add_dependencies(cadmium_tests cadmium_reviewview_test)
    set_tests_properties(cadmium_reviewview PROPERTIES TIMEOUT 120)

    set_tests_properties(
        cadmium_toonzraster cadmium_fullcolor cadmium_vector cadmium_codec
        cadmium_settings cadmium_metadata cadmium_save cadmium_destination cadmium_color
        PROPERTIES TIMEOUT 120)
endif()

if(TARGET ui_qt_catalogs)
    add_dependencies(OpenToonz ui_qt_catalogs)
endif()
if(BUILD_CADMIUM_TESTS)
    target_link_libraries(cadmium_vector_test PRIVATE toonzqt)
    target_link_libraries(cadmium_settings_test PRIVATE toonzqt)
    target_link_libraries(cadmium_reviewview_test PRIVATE toonzqt)
    target_link_libraries(cadmium_pipeline_test PRIVATE toonzqt)
endif()

# Shared native UI ports; use the same objects and behavior in CI as in the app.
target_sources(OpenToonz PRIVATE mediacachepopup.cpp mediacachepopup.h)
target_link_libraries(OpenToonz spdlog::spdlog_header_only Threads::Threads)
if(WIN32)
    target_compile_definitions(OpenToonz PRIVATE SPDLOG_WCHAR_FILENAMES)
endif()

if(BUILD_UI_PORT_TESTS)
    target_sources(OpenToonz PRIVATE nativeuiintegrationtest.cpp nativeuiintegrationtest.h)
    target_sources(OpenToonz PRIVATE artistworkflowbenchmark.cpp artistworkflowbenchmark.h)
    target_sources(OpenToonz PRIVATE mediacacheintegrationtest.cpp mediacacheintegrationtest.h)
    if(WIN32)
        target_link_libraries(OpenToonz psapi)
    endif()
    target_compile_definitions(OpenToonz PRIVATE BUILD_UI_PORT_TESTS)
    target_compile_definitions(toonzlib PRIVATE BUILD_UI_PORT_TESTS)
    add_custom_target(ui_port_tests)
    function(add_ui_port_test name)
        add_executable(ui_${name}_test ${ARGN})
        target_include_directories(ui_${name}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(ui_${name}_test PRIVATE
            Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
        if(name STREQUAL "themeapplication")
            add_test(NAME ui_${name} COMMAND ui_${name}_test "${CMAKE_SOURCE_DIR}/../../stuff/config/qss")
        else()
            add_test(NAME ui_${name} COMMAND ui_${name}_test)
        endif()
        set_tests_properties(ui_${name} PROPERTIES TIMEOUT 120)
        add_dependencies(ui_port_tests ui_${name}_test)
    endfunction()
    add_ui_port_test(commandpalette tests/commandpalette_test.cpp commandpalette.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_commandpalette.cpp)
    add_ui_port_test(widgetinspector tests/widgetinspector_test.cpp widgetinspector.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_widgetinspector.cpp)
    add_ui_port_test(themeapplication tests/themeapplication_test.cpp themeapplication.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_themeapplication.cpp)
    add_ui_port_test(themeproperties tests/themeproperties_test.cpp)
    add_ui_port_test(mediacache tests/mediacache_test.cpp)
    target_compile_definitions(ui_mediacache_test PRIVATE MEDIA_CACHE_TEST_HOOKS)
    target_link_libraries(ui_mediacache_test PRIVATE Threads::Threads)
    target_link_libraries(ui_themeproperties_test PRIVATE toonzqt)
    add_ui_port_test(uitranslations tests/uitranslations_test.cpp)
    target_link_libraries(ui_uitranslations_test PRIVATE toonzqt)
    if(NOT OT_QT_LRELEASE_EXECUTABLE)
        if(QT_VERSION_MAJOR EQUAL 6)
            set(OT_QT_LRELEASE_EXECUTABLE Qt6::lrelease)
        else()
            set(OT_QT_LRELEASE_EXECUTABLE ${Qt5_LRELEASE_EXECUTABLE})
        endif()
    endif()
    set(ui_language_fixture_root "${CMAKE_CURRENT_BINARY_DIR}/ui-language-fixtures")
    set(ui_language_fixture_outputs)
    foreach(fixture a b)
        if(fixture STREQUAL "a")
            set(fixture_directory "${ui_language_fixture_root}/FixtureA")
        else()
            set(fixture_directory "${ui_language_fixture_root}/FixtureB")
        endif()
        set(fixture_compiled "${fixture_directory}/toonz.qm")
        add_custom_command(OUTPUT "${fixture_compiled}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${fixture_directory}"
            COMMAND ${OT_QT_LRELEASE_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ui_language_${fixture}.ts"
                -qm "${fixture_compiled}"
            DEPENDS "tests/fixtures/ui_language_${fixture}.ts" VERBATIM)
        list(APPEND ui_language_fixture_outputs "${fixture_compiled}")
        foreach(module toonzqt tnzcore toonzlib colorfx tnztools image qtbase)
            set(fixture_copy "${fixture_directory}/${module}.qm")
            add_custom_command(OUTPUT "${fixture_copy}"
                COMMAND ${CMAKE_COMMAND} -E copy "${fixture_compiled}" "${fixture_copy}"
                DEPENDS "${fixture_compiled}" VERBATIM)
            list(APPEND ui_language_fixture_outputs "${fixture_copy}")
        endforeach()
    endforeach()
    add_custom_target(ui_language_fixtures DEPENDS ${ui_language_fixture_outputs})
    add_executable(ui_languagemanager_test tests/languagemanager_test.cpp languagemanager.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_languagemanager.cpp)
    target_include_directories(ui_languagemanager_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ui_languagemanager_test PRIVATE
        Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets toonzqt)
    add_dependencies(ui_languagemanager_test ui_language_fixtures)
    add_dependencies(ui_port_tests ui_languagemanager_test)
    if(WITH_TRANSLATION)
        add_test(NAME ui_languagemanager COMMAND ui_languagemanager_test
            "${ui_language_fixture_root}" "${CMAKE_SOURCE_DIR}/../../stuff/config/loc")
    else()
        add_test(NAME ui_languagemanager COMMAND ui_languagemanager_test "${ui_language_fixture_root}")
    endif()
    set_tests_properties(ui_languagemanager PROPERTIES TIMEOUT 120)
    add_ui_port_test(messagehistory tests/messagehistory_test.cpp)
    target_link_libraries(ui_messagehistory_test PRIVATE toonzqt tnzcore Threads::Threads)
    add_ui_port_test(diagnosticswriter tests/nativediagnosticswriter_test.cpp
        nativediagnosticswriter.cpp)
    add_ui_port_test(diagnosticsservice tests/nativediagnostics_service_test.cpp
        nativediagnosticswriter.cpp nativediagnostics.cpp diagnosticssettingswidget.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_nativediagnostics.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/moc_diagnosticssettingswidget.cpp)
    target_link_libraries(ui_diagnosticsservice_test PRIVATE toonzqt tnzcore)
    foreach(diagnostic_target ui_diagnosticswriter_test ui_diagnosticsservice_test)
        target_link_libraries(${diagnostic_target} PRIVATE spdlog::spdlog_header_only Threads::Threads)
        if(WIN32)
            target_compile_definitions(${diagnostic_target} PRIVATE SPDLOG_WCHAR_FILENAMES)
        endif()
    endforeach()

endif()

if(BUILD_RELIABILITY_TESTS)
    add_executable(levelselection_test tests/levelselection_test.cpp)
    target_include_directories(levelselection_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(levelselection_test PRIVATE
        toonzlib tnzbase tnzcore Qt${QT_VERSION_MAJOR}::Core)
    add_test(NAME levelselection COMMAND levelselection_test)
endif()

option(BUILD_PROCESS_PROGRESS_TESTS "Build the external tool wait test" ${BUILD_TESTS})
if(BUILD_PROCESS_PROGRESS_TESTS)
    add_executable(processprogress_test tests/processprogress_test.cpp)
    target_include_directories(processprogress_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(processprogress_test Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
    add_test(NAME processprogress COMMAND processprogress_test)
    set_tests_properties(processprogress PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(BUILD_SEEXPR_TESTS)
    target_sources(OpenToonz PRIVATE seexprnativeintegrationtest.cpp seexprnativeintegrationtest.h)
    target_compile_definitions(OpenToonz PRIVATE BUILD_SEEXPR_TESTS)
    target_compile_definitions(toonzlib PRIVATE BUILD_SEEXPR_TESTS)
    add_test(NAME seexpr_native_application
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../ci-scripts/seexpr/native_ui_test.py"
            "$<TARGET_FILE:OpenToonz>"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../stuff"
            --output "${CMAKE_BINARY_DIR}/seexpr-native-output.txt"
            --windows-crash-debugger)
    set_tests_properties(seexpr_native_application PROPERTIES TIMEOUT 360)
endif()
