# Development deployment of Qt plugins next to an executable, so Debug and
# Release builds start straight from the build tree. (vcpkg's app-local step
# already copies the Qt DLLs; plugins are not copied by it.) The release
# package uses windeployqt instead (see src/app/CMakeLists.txt).

function(wizz_copy_qt_plugins target)
    if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
        message(WARNING "wizz_copy_qt_plugins: vcpkg variables are not set; skipping plugin copy for ${target}")
        return()
    endif()
    set(_root "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    set(_plugins "${_root}/$<$<CONFIG:Debug>:debug/>Qt6/plugins")
    foreach(_dir IN ITEMS platforms styles imageformats)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                    "${_plugins}/${_dir}" "$<TARGET_FILE_DIR:${target}>/${_dir}"
            VERBATIM)
    endforeach()
endfunction()
