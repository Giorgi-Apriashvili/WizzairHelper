# Desktop application (01), split into a widget library and a thin executable
# so that GUI tests (tests/gui) can link the widgets.
#
#   wizzairhelper_app (wizz::app): every widget, the controller, autotest runner,
#                                  crash reporter — sources under lib/
#   WizzairHelper.exe            : main.cpp + version resource + application manifest
#
# Owns all widgets, the active job ID, explicit fare selection, timers,
# settings, diagnostics and the hidden --autotest mode. Never blocks the UI
# thread (06 §2). Pure rules live in wizz::ui_logic (Qt-free).

find_package(Qt6 6.11 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()
include(WizzQtDeploy)

wizz_collect_sources(WIZZ_APP_LIB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/*.cpp")

qt_add_library(wizzairhelper_app STATIC ${WIZZ_APP_LIB_SOURCES})
add_library(wizz::app ALIAS wizzairhelper_app)

target_include_directories(wizzairhelper_app PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/lib")

target_compile_definitions(wizzairhelper_app PUBLIC
    WIZZ_VERSION_STRING="${PROJECT_VERSION}"
    QT_NO_KEYWORDS)

target_link_libraries(wizzairhelper_app
    PUBLIC  wizz::core
            wizz::services
            wizz::ui_logic
            Qt6::Widgets
    PRIVATE ole32           # ITaskbarList3 (COM)
            windowscodecs   # WIC PNG encoder for --autotest screenshots
            dwmapi)

# The .manifest source is merged into the linker's manifest and embedded (per-monitor-v2
# DPI awareness before anything is shown, 01 §3 / 06 §4). It also replaces Qt's generated
# default manifest, which the Visual Studio generator links with /MANIFEST:NO and so
# never embeds.
qt_add_executable(WizzairHelper WIN32
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/WizzairHelper.rc"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/WizzairHelper.manifest")

target_link_libraries(WizzairHelper PRIVATE wizz::app)
set_target_properties(WizzairHelper PROPERTIES OUTPUT_NAME "WizzairHelper")

wizz_copy_qt_plugins(WizzairHelper)

# Release package (06 §4): install rules (windeployqt, runtime DLLs, app-local MSVC
# runtime, notices) and the CPack ZIP behind `cmake --build --preset package`.
include(WizzPackaging)
wizz_add_release_package(WizzairHelper)
include(CPack)
