# Common compile settings for every WizzairHelper target, applied by linking
# the interface target `wizz::project_options`.

add_library(wizz_project_options INTERFACE)
add_library(wizz::project_options ALIAS wizz_project_options)

target_compile_features(wizz_project_options INTERFACE cxx_std_23)

target_compile_definitions(wizz_project_options INTERFACE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00)

# MSVC and clang-cl share the cl-style command line (MSVC is true for both).
if(MSVC)
    target_compile_options(wizz_project_options INTERFACE
        /W4                 # high warning level
        /permissive-        # standards conformance
        /utf-8              # UTF-8 sources and execution charset (06 §4)
        /Zc:__cplusplus
        /Zc:inline
        /EHsc
        /bigobj
        /diagnostics:caret)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # Options clang-cl does not implement (it would warn "argument unused").
        target_compile_options(wizz_project_options INTERFACE
            /Zc:preprocessor
            /Zc:throwingNew
            /MP)            # parallel compilation with the Visual Studio generator
    else()
        # clang-cl: designated initializers that leave members to their default
        # member initializers are intended (e.g. Airport{.iata = "LTN", .name = ...}).
        target_compile_options(wizz_project_options INTERFACE
            -Wno-missing-designated-field-initializers)
    endif()
    if(WIZZ_WARNINGS_AS_ERRORS)
        target_compile_options(wizz_project_options INTERFACE /WX)
    endif()
endif()

# Sources are collected per target with CONFIGURE_DEPENDS globs so that new
# files are picked up on the next build without editing CMake lists.
function(wizz_collect_sources out_var)
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS ${ARGN})
    set(${out_var} ${_sources} PARENT_SCOPE)
endfunction()
