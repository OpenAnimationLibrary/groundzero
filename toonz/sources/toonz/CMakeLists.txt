set(VERSION 1.8)

set(MOC_HEADERS
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
    drawingsubstitutionpane.h
    substitutionsliderpane.h
    storyboardpane.h
    mastercontrollerspane.h
    offsetlayerspane.h
    artlayermanagerpane.h
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
    ${MOC_HEADERS}
    celldata.h
    cellkeyframedata.h
    cellkeyframeselection.h
    cellselection.h
    columncommand.h
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
    tfarmstuff.h
    versioncontrolgui.h
    versioncontrolxmlreader.h
    viewerdraw.h
    aspectguides.h
    pegbarguides.h
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
# Tracker file
    ObjectTracker.h
    dummyprocessor.h
    metnum.h
    processor.h
    predict3d.h
    tvpjson_io.h
)

set(SOURCES
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
    drawingsubstitutionpane.cpp
    drawingsubstitutionundo.cpp
    substitutionsliderpane.cpp
    storyboardpane.cpp
    mastercontrollerspane.cpp
    offsetlayerspane.cpp
    artlayermanagerpane.cpp
    createcolourartcommand.cpp
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
    abrimport.cpp
    deformerbake.cpp
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
