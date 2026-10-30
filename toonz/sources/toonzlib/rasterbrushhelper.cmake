# Shared by the application and standalone adapter qualification builds.
# No app-sandbox inheritance: the drawing application itself is unrestricted.
set(_brush_helper_dir "${CMAKE_CURRENT_LIST_DIR}")
function(sign_rasterbrush_helper target)
  find_program(BRUSH_CODESIGN codesign REQUIRED)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${BRUSH_CODESIGN}" --force --sign -
      --identifier org.openanimationlibrary.groundzero.brush-import
      --entitlements "${_brush_helper_dir}/resources/rasterbrushimport.entitlements"
      "$<TARGET_FILE:${target}>"
    VERBATIM)
endfunction()
function(add_rasterbrush_helper target qt_major main_source)
  add_executable(${target}
    "${main_source}"
    "${_brush_helper_dir}/rasterbrushpreset.cpp"
    "${_brush_helper_dir}/rasterbrushimport.cpp"
    "${_brush_helper_dir}/rasterbrushworker.cpp"
    "${_brush_helper_dir}/rasterbrushpng.cpp"
    "${_brush_helper_dir}/rasterbrusharchive.cpp"
    "${_brush_helper_dir}/abrparser.cpp"
    "${_brush_helper_dir}/rasterbrushengine.cpp")
  target_include_directories(${target} PRIVATE "${_brush_helper_dir}/../include")
  target_compile_definitions(${target} PRIVATE TOONZLIB_EXPORTS MACOSX)
  target_link_libraries(${target} PRIVATE Qt${qt_major}::Core Qt${qt_major}::Gui
    ZLIB::ZLIB "-framework Security" "-framework CoreFoundation")
  sign_rasterbrush_helper(${target})
endfunction()
