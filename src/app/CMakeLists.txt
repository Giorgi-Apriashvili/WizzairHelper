# WizzairHelper.exe: Qt 6 Widgets desktop application (01). Owns all widgets,
# the active job ID, explicit fare selection, timers, settings, diagnostics
# and the hidden --autotest mode. Never blocks the UI thread (06 §2).

find_package(Qt6 6.11 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

wizz_collect_sources(WIZZ_APP_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

qt_add_executable(WizzairHelper WIN32
    ${WIZZ_APP_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/WizzairHelper.rc")

target_include_directories(WizzairHelper PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(WizzairHelper PRIVATE
    WIZZ_VERSION_STRING="${PROJECT_VERSION}"
    QT_NO_KEYWORDS)

target_link_libraries(WizzairHelper PRIVATE
    wizz::core
    wizz::services
    wizz::ui_logic
    Qt6::Widgets
    ole32           # ITaskbarList3 (COM)
    windowscodecs   # WIC PNG encoder for --autotest screenshots
    dwmapi)

set_target_properties(WizzairHelper PROPERTIES OUTPUT_NAME "WizzairHelper")
