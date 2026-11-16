# The `dist` target's work (cmake/WizzPackaging.cmake, D-38), run in script mode at build time:
#
#   1. stage the install rules' files: cmake --install <build> --prefix
#      <stage>/WizzairHelper-<version>-win64
#   2. pack that folder into <dist>/WizzairHelper-<version>-win64.zip (the folder is the ZIP's
#      single top-level entry)
#   3. compile packaging/WizzairHelper.iss with ISCC into
#      <dist>/WizzairHelper-<version>-win64-setup.exe, which installs exactly the staged files
#
# <dist> and <stage> are emptied first, so <dist> holds these two files and nothing else (CI
# adds SHA256SUMS.txt). Only the Release configuration is packaged.

foreach(_variable IN ITEMS WIZZ_CONFIG WIZZ_BUILD_DIR WIZZ_VERSION WIZZ_ISS WIZZ_STAGE_DIR WIZZ_DIST_DIR)
    if(NOT DEFINED ${_variable} OR "${${_variable}}" STREQUAL "")
        message(FATAL_ERROR "WizzDist.cmake: -D${_variable}=... is missing")
    endif()
endforeach()

if(NOT WIZZ_CONFIG STREQUAL "Release")
    message(FATAL_ERROR
        "dist packages the Release build, not ${WIZZ_CONFIG}: run `cmake --build --preset dist`.")
endif()
if(NOT WIZZ_ISCC OR NOT EXISTS "${WIZZ_ISCC}")
    message(FATAL_ERROR
        "dist builds the installer with Inno Setup 6, and its compiler ISCC.exe was not found (looked on "
        "PATH, in C:/Program Files (x86)/Inno Setup 6 and in %LOCALAPPDATA%/Programs/Inno Setup 6). "
        "Install Inno Setup 6 (winget install JRSoftware.InnoSetup) and run the configure step again, "
        "or configure with -DWIZZ_ISCC=<path of ISCC.exe>.")
endif()

set(_name "WizzairHelper-${WIZZ_VERSION}-win64")
set(_package "${WIZZ_STAGE_DIR}/${_name}")
set(_zip "${WIZZ_DIST_DIR}/${_name}.zip")
set(_setup "${WIZZ_DIST_DIR}/${_name}-setup.exe")

file(REMOVE_RECURSE "${WIZZ_STAGE_DIR}" "${WIZZ_DIST_DIR}")
file(MAKE_DIRECTORY "${WIZZ_STAGE_DIR}" "${WIZZ_DIST_DIR}")

message(STATUS "dist: staging ${_package}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${WIZZ_BUILD_DIR}" --config Release --prefix "${_package}"
    COMMAND_ERROR_IS_FATAL ANY)

message(STATUS "dist: packing ${_zip}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${_zip}" --format=zip -- "${_name}"
    WORKING_DIRECTORY "${WIZZ_STAGE_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)

# ISCC takes native paths; each /D value is one argument, quoted by execute_process if needed.
file(TO_NATIVE_PATH "${_package}" _package_native)
file(TO_NATIVE_PATH "${WIZZ_DIST_DIR}" _dist_native)
file(TO_NATIVE_PATH "${WIZZ_ISS}" _iss_native)
message(STATUS "dist: compiling ${_setup}")
execute_process(
    COMMAND "${WIZZ_ISCC}" /Q
            "/DWizzVersion=${WIZZ_VERSION}"
            "/DWizzStageDir=${_package_native}"
            "/DWizzOutputDir=${_dist_native}"
            "/DWizzOutputName=${_name}-setup"
            "${_iss_native}"
    COMMAND_ERROR_IS_FATAL ANY)

foreach(_artefact IN ITEMS "${_zip}" "${_setup}")
    if(NOT EXISTS "${_artefact}")
        message(FATAL_ERROR "dist: ${_artefact} was not written")
    endif()
    file(SIZE "${_artefact}" _size)
    message(STATUS "dist: ${_artefact} (${_size} bytes)")
endforeach()
