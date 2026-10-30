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
function(deploy_rasterbrush_windows target qt_major directory)
  if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(runtime_base "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
  else()
    set(runtime_base "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
    "${runtime_base}/$<CONFIG>/${directory}")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
      "-DHELPER=$<TARGET_FILE:${target}>"
      "-DQT_LIBRARY=$<TARGET_FILE:Qt${qt_major}::Core>"
      "-DBRUSH_CONFIGURATION=$<CONFIG>"
      "-DBRUSH_ARCH=${CMAKE_VS_PLATFORM_NAME}"
      "-DBRUSH_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DBRUSH_WINDOWS_SDK_VERSION=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
      -P "${_brush_helper_dir}/deploybrushwindows.cmake"
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
  target_compile_definitions(${target} PRIVATE TOONZLIB_EXPORTS)
  target_link_libraries(${target} PRIVATE Qt${qt_major}::Core Qt${qt_major}::Gui)
  if(TARGET ZLIB::ZLIB)
    target_link_libraries(${target} PRIVATE ZLIB::ZLIB)
  else()
    target_link_libraries(${target} PRIVATE ${Z_LIB})
  endif()
  if(APPLE)
    target_compile_definitions(${target} PRIVATE MACOSX)
    target_link_libraries(${target} PRIVATE "-framework Security" "-framework CoreFoundation")
    sign_rasterbrush_helper(${target})
  elseif(WIN32)
    target_link_libraries(${target} PRIVATE userenv advapi32 ws2_32)
    if(target STREQUAL "rasterbrushworker_test")
      deploy_rasterbrush_windows(${target} ${qt_major} "brush-worker-test-runtime")
    else()
      deploy_rasterbrush_windows(${target} ${qt_major} "brush-import-runtime")
    endif()
  endif()
endfunction()
