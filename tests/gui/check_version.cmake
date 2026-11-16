# CTest app.version (D-35): runs WizzairHelper.exe --version with standard output and
# standard error read through pipes, as scripts and updaters do, and checks that it prints
# exactly "WizzairHelper <VERSION>" and a line feed, nothing on standard error, and exits
# with 0 (well before the timeout: no window, no event loop).
#
#   cmake -DEXE=<path of WizzairHelper.exe> -DVERSION=<X.Y.Z> -P check_version.cmake

foreach(_variable IN ITEMS EXE VERSION)
    if(NOT DEFINED ${_variable} OR "${${_variable}}" STREQUAL "")
        message(FATAL_ERROR "check_version.cmake needs -D${_variable}=...")
    endif()
endforeach()

execute_process(
    COMMAND "${EXE}" --version
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _output
    ERROR_VARIABLE _error
    TIMEOUT 30)

set(_expected "WizzairHelper ${VERSION}\n")
if(NOT _result STREQUAL "0")
    message(FATAL_ERROR "WizzairHelper.exe --version exited with '${_result}' (expected 0); output: [${_output}]")
endif()
if(NOT _output STREQUAL _expected)
    message(FATAL_ERROR "WizzairHelper.exe --version printed [${_output}], expected [${_expected}]")
endif()
if(NOT _error STREQUAL "")
    message(FATAL_ERROR "WizzairHelper.exe --version wrote to standard error: [${_error}]")
endif()
message(STATUS "WizzairHelper.exe --version: ${_output}")
