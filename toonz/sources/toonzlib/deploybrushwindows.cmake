# Build/package-time only. Never run deployment tools or download code at import.
get_filename_component(runtime "${HELPER}" DIRECTORY)
get_filename_component(qt_bin "${QT_LIBRARY}" DIRECTORY)
find_program(WINDEPLOYQT windeployqt HINTS "${qt_bin}" REQUIRED)
# This target owns a dedicated runtime directory. Rebuild its deployed DLLs on
# every link so old third-party binaries cannot shadow upgraded dependencies.
# Keep the newly linked executable and its debugging symbols intact.
file(GLOB_RECURSE previous_runtime_dlls "${runtime}/*.dll")
if(previous_runtime_dlls)
    file(REMOVE ${previous_runtime_dlls})
endif()
file(REMOVE "${runtime}/runtime.files" "${runtime}/qt.conf")
# Debug runtimes are for the local licensed development/CI machine only. They
# are not provided by the release VC Redistributable and are not distributed.
if(BRUSH_CONFIGURATION STREQUAL "Debug")
    # MSBuild custom commands need not inherit a developer-prompt environment.
    # Derive VCINSTALLDIR from the configured compiler, not a different VS install.
    if(NOT DEFINED ENV{VCINSTALLDIR})
        file(TO_CMAKE_PATH "${BRUSH_CXX_COMPILER}" compiler_path)
        if(compiler_path MATCHES "^(.*)/Tools/MSVC/[^/]+/bin/")
            set(ENV{VCINSTALLDIR} "${CMAKE_MATCH_1}/")
        endif()
    endif()
    set(compiler_runtime_options --debug --compiler-runtime)
else()
    set(compiler_runtime_options --release --no-compiler-runtime)
endif()
execute_process(COMMAND "${WINDEPLOYQT}" --no-translations ${compiler_runtime_options}
    --no-opengl-sw "${HELPER}" RESULT_VARIABLE deployed)
if(NOT deployed EQUAL 0)
    message(FATAL_ERROR "Cannot deploy the isolated brush Qt runtime")
endif()
# windeployqt does not cover arbitrary third-party DLLs (notably dynamic zlib).
# Resolve those against the build's trusted library paths, never at user runtime.
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "dumpbin")
find_program(BRUSH_DUMPBIN dumpbin REQUIRED)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${BRUSH_DUMPBIN}")
file(TO_CMAKE_PATH "$ENV{PATH}" dependency_paths)
if(BRUSH_CONFIGURATION STREQUAL "Debug")
    if(BRUSH_ARCH STREQUAL "Win32")
        set(BRUSH_ARCH "x86")
    elseif(NOT BRUSH_ARCH)
        # The supported standalone Windows CI toolchain is x64 MSVC.
        set(BRUSH_ARCH "x64")
    endif()
    string(TOLOWER "${BRUSH_ARCH}" BRUSH_ARCH)
    file(TO_CMAKE_PATH "$ENV{UniversalCRTSdkDir}" ucrt_sdk)
    file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" windows_sdk)
    file(TO_CMAKE_PATH "$ENV{UCRTVersion}" ucrt_version)
    file(TO_CMAKE_PATH "$ENV{WindowsSDKVersion}" windows_version)
    if(NOT windows_sdk)
        get_filename_component(windows_sdk
            "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]"
            ABSOLUTE)
    endif()
    if(NOT windows_version)
        set(windows_version "${BRUSH_WINDOWS_SDK_VERSION}")
    endif()
    set(debug_ucrt_paths
        "${ucrt_sdk}/bin/${ucrt_version}/${BRUSH_ARCH}/ucrt"
        "${windows_sdk}/bin/${windows_version}/${BRUSH_ARCH}/ucrt")
    find_file(BRUSH_DEBUG_UCRT ucrtbased.dll PATHS ${debug_ucrt_paths} NO_DEFAULT_PATH)
    if(NOT BRUSH_DEBUG_UCRT)
        message(FATAL_ERROR "Debug brush helper requires the selected Windows SDK debug UCRT")
    endif()
    file(COPY "${BRUSH_DEBUG_UCRT}" DESTINATION "${runtime}")
    # Do not pre-exclude CRT names: missing DebugCRT must fail dependency closure.
    set(crt_exclusions)
else()
    set(crt_exclusions "^[mM][sS][vV][cC][pP]"
        "^[vV][cC][rR][uU][nN][tT][iI][mM][eE]" "^[cC][oO][nN][cC][rR][tT]")
endif()
file(GLOB_RECURSE deployed_dlls "${runtime}/*.dll")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${HELPER}" LIBRARIES ${deployed_dlls}
    DIRECTORIES "${runtime}" "${qt_bin}" ${dependency_paths}
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    PRE_EXCLUDE_REGEXES "^[aA][pP][iI]-" "^[eE][xX][tT]-"
        ${crt_exclusions}
    POST_EXCLUDE_REGEXES "[/\\\\][wW][iI][nN][dD][oO][wW][sS][/\\\\]")
if(unresolved)
    message(FATAL_ERROR "Unresolved brush helper dependencies: ${unresolved}")
endif()
foreach(dependency IN LISTS dependencies)
    get_filename_component(name "${dependency}" NAME)
    if(NOT EXISTS "${runtime}/${name}")
        file(COPY "${dependency}" DESTINATION "${runtime}")
    endif()
endforeach()
# Keep plugin discovery relative to this self-contained runtime after private copy.
file(WRITE "${runtime}/qt.conf" "[Paths]\nPrefix=.\nPlugins=.\n")
file(GLOB_RECURSE runtime_files RELATIVE "${runtime}"
    "${runtime}/*.exe" "${runtime}/*.dll" "${runtime}/qt.conf")
list(SORT runtime_files)
file(WRITE "${runtime}/runtime.files" "")
set(total 0)
foreach(relative IN LISTS runtime_files)
    if(NOT relative MATCHES "^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$")
        message(FATAL_ERROR "Invalid isolated runtime filename: ${relative}")
    endif()
    string(TOLOWER "${relative}" lower_name)
    if(NOT BRUSH_CONFIGURATION STREQUAL "Debug" AND
       (lower_name MATCHES "(^|/)(msvcp|vcruntime|concrt)[0-9_a-z]*d[.]dll$" OR
        lower_name MATCHES "(^|/)ucrtbased[.]dll$"))
        message(FATAL_ERROR "Non-redistributable Debug CRT found in release brush runtime")
    endif()
    file(SIZE "${runtime}/${relative}" bytes)
    math(EXPR total "${total} + ${bytes}")
    if(bytes GREATER 134217728 OR total GREATER 536870912)
        message(FATAL_ERROR "Isolated brush runtime exceeds staging size bounds")
    endif()
    file(SHA256 "${runtime}/${relative}" digest)
    file(APPEND "${runtime}/runtime.files" "${digest} ${relative}\n")
endforeach()
list(LENGTH runtime_files count)
if(count GREATER 256)
    message(FATAL_ERROR "Isolated brush runtime exceeds staging file-count bound")
endif()
message(STATUS "Isolated brush runtime: ${count} files, ${total} bytes")
