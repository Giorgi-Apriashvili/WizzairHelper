# Release package of WizzairHelper.exe (06 §4): install rules, and the `dist` target that
# turns them into the release artefacts (D-38).
#
#   <prefix>/WizzairHelper.exe
#           /Qt6Core.dll, Qt6Gui.dll, Qt6Widgets.dll         (windeployqt)
#           /platforms/qwindows.dll, styles/qmodernwindowsstyle.dll (windowsvista),
#            imageformats/                                     (windeployqt)
#           /libcurl.dll, z.dll, brotlicommon.dll, brotlidec.dll, pcre2-16.dll,
#            double-conversion.dll, libpng16.dll, md4c.dll    (runtime dependency scan)
#           /msvcp140*.dll, vcruntime140*.dll, concrt140.dll  (app-local MSVC runtime;
#                                                              the UCRT ships with Windows 10/11)
#           /LICENSE.txt (WizzairHelper, MIT), THIRD-PARTY-NOTICES.txt, README.txt,
#            licenses/<port>.txt
#
# windeployqt is vcpkg's tools/Qt6/bin/windeployqt.exe. Qt's network-only plugin
# types (generic = TUIO touch over UDP, networkinformation, tls) are skipped: the app
# does its HTTP through libcurl and never loads Qt Network. So are the Direct3D shader
# compilers (d3dcompiler, dxcompiler/dxil from the Windows SDK) and the software
# OpenGL: a Widgets app paints with the raster engine. ICU is the Windows system
# icu.dll (02 §4.3) and is not packaged.
#
# `cmake --build --preset dist` (target `dist`, Release only) stages these files with
# `cmake --install` and writes, to <build>/dist (cmake/WizzDist.cmake):
#   WizzairHelper-<version>-win64.zip        portable: the staged files under a top-level
#                                            WizzairHelper-<version>-win64 folder
#   WizzairHelper-<version>-win64-setup.exe  Inno Setup per-user installer of exactly those
#                                            files (packaging/WizzairHelper.iss)
# The installer needs Inno Setup 6's ISCC.exe (WIZZ_ISCC); without it `dist` fails and says so.

function(wizz_add_release_package target)
    if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
        message(WARNING "wizz_add_release_package: vcpkg variables are not set; no install rules for ${target}")
        return()
    endif()
    set(_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    find_program(WIZZ_WINDEPLOYQT windeployqt
        HINTS "${_root}/tools/Qt6/bin"
        NO_DEFAULT_PATH
        DOC "windeployqt of the Qt used for the build (vcpkg tools/Qt6/bin)")
    if(NOT WIZZ_WINDEPLOYQT)
        message(WARNING "windeployqt not found under ${_root}/tools/Qt6/bin; the package will lack the Qt files")
    endif()

    install(TARGETS ${target} RUNTIME DESTINATION .)

    # 1. Qt libraries and plugins.
    if(WIZZ_WINDEPLOYQT)
        install(CODE "
            set(_prefix \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
            message(STATUS \"windeployqt: \${_prefix}/$<TARGET_FILE_NAME:${target}>\")
            # The output is kept unless it fails: vcpkg's Qt has no translation catalogs, and
            # windeployqt reports that even with --no-translations.
            execute_process(
                COMMAND \"${WIZZ_WINDEPLOYQT}\" $<IF:$<CONFIG:Debug>,--debug,--release>
                        --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw
                        --no-compiler-runtime
                        --skip-plugin-types generic,networkinformation,tls
                        --verbose 0
                        --dir \"\${_prefix}\" \"\${_prefix}/$<TARGET_FILE_NAME:${target}>\"
                RESULT_VARIABLE _result
                OUTPUT_VARIABLE _output
                ERROR_VARIABLE _output)
            if(NOT _result EQUAL 0)
                message(FATAL_ERROR \"windeployqt failed (\${_result}):\\n\${_output}\")
            endif()
        ")
    endif()

    # 2. Every non-system DLL the exe and the deployed plugins load (libcurl, zlib,
    #    brotli and Qt's own dependencies), resolved in vcpkg's bin directory.
    install(CODE "
        if(POLICY CMP0207)
            cmake_policy(SET CMP0207 NEW)  # normalise paths before the exclude regexes
        endif()
        set(_prefix \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
        file(GLOB_RECURSE _libraries \"\${_prefix}/*.dll\")
        file(GET_RUNTIME_DEPENDENCIES
            EXECUTABLES \"\${_prefix}/$<TARGET_FILE_NAME:${target}>\"
            LIBRARIES \${_libraries}
            DIRECTORIES \"${_root}/$<$<CONFIG:Debug>:debug/>bin\"
            PRE_EXCLUDE_REGEXES \"^[Aa][Pp][Ii]-[Mm][Ss]-\" \"^[Ee][Xx][Tt]-[Mm][Ss]-\"
            POST_EXCLUDE_REGEXES \"[/\\\\\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\\\\\]\"
            RESOLVED_DEPENDENCIES_VAR _resolved
            UNRESOLVED_DEPENDENCIES_VAR _unresolved
            CONFLICTING_DEPENDENCIES_PREFIX _conflicting)
        # A DLL found both next to the exe (windeployqt put it there) and in vcpkg's bin.
        foreach(_name IN LISTS _conflicting_FILENAMES)
            list(GET _conflicting_\${_name} 0 _first)
            list(APPEND _resolved \"\${_first}\")
        endforeach()
        foreach(_dll IN LISTS _resolved)
            get_filename_component(_name \"\${_dll}\" NAME)
            if(NOT EXISTS \"\${_prefix}/\${_name}\")
                message(STATUS \"Runtime dependency: \${_name}\")
                file(COPY \"\${_dll}\" DESTINATION \"\${_prefix}\")
            endif()
        endforeach()
        foreach(_dll IN LISTS _unresolved)
            if(NOT _dll MATCHES \"^(msvcp|vcruntime|concrt|ucrtbase)\")
                message(WARNING \"Unresolved runtime dependency: \${_dll}\")
            endif()
        endforeach()
    ")

    # 3. The MSVC runtime, app-local (the package starts without the VC++ redistributable).
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION .)
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)       # part of Windows 10 and 11
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP OFF)
    include(InstallRequiredSystemLibraries)
    if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        message(WARNING "InstallRequiredSystemLibraries found no MSVC runtime; the package needs the VC++ redistributable")
    endif()

    # 4. Notices: the app's own license, the summary, and each bundled component's license
    #    as vcpkg ships it.
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION . RENAME LICENSE.txt)
    install(FILES
        "${PROJECT_SOURCE_DIR}/packaging/THIRD-PARTY-NOTICES.txt"
        "${PROJECT_SOURCE_DIR}/packaging/README.txt"
        DESTINATION .)
    foreach(_port IN ITEMS qtbase curl zlib brotli pcre2 double-conversion libpng md4c)
        if(EXISTS "${_root}/share/${_port}/copyright")
            install(FILES "${_root}/share/${_port}/copyright" DESTINATION licenses RENAME "${_port}.txt")
        else()
            message(WARNING "No license file for ${_port} under ${_root}/share/${_port}")
        endif()
    endforeach()

    # 5. `dist`: the portable ZIP and the installer, built from the rules above at build time.
    find_program(WIZZ_ISCC NAMES ISCC iscc
        HINTS "C:/Program Files (x86)/Inno Setup 6"
              "$ENV{ProgramFiles}/Inno Setup 6"
              "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
        DOC "Inno Setup 6 command-line compiler (ISCC.exe) for the installer of the dist target")
    if(NOT WIZZ_ISCC)
        message(STATUS "Inno Setup 6 (ISCC.exe) not found: the dist target will fail until it is installed")
    endif()
    add_custom_target(dist
        COMMAND "${CMAKE_COMMAND}"
                "-DWIZZ_CONFIG=$<CONFIG>"
                "-DWIZZ_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DWIZZ_VERSION=${PROJECT_VERSION}"
                "-DWIZZ_ISCC=${WIZZ_ISCC}"
                "-DWIZZ_ISS=${PROJECT_SOURCE_DIR}/packaging/WizzairHelper.iss"
                "-DWIZZ_STAGE_DIR=${CMAKE_BINARY_DIR}/dist-stage"
                "-DWIZZ_DIST_DIR=${CMAKE_BINARY_DIR}/dist"
                -P "${PROJECT_SOURCE_DIR}/cmake/WizzDist.cmake"
        COMMENT "Release artefacts: portable ZIP and installer in ${CMAKE_BINARY_DIR}/dist"
        VERBATIM)
    add_dependencies(dist ${target})
endfunction()
