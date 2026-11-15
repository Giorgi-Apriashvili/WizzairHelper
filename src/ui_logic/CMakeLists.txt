# wizzairhelper_ui_logic: the Qt-free presenter / view-model layer of the
# desktop app (01 §4-§13, §17, §18; 05 §7.2 U-01..U-11). Pure, window-free
# units the Qt widgets delegate to: the string catalogue, leg status texts,
# the BusyPanel state machine, date and nights coupling, request building,
# settings persistence, --autotest options (+ the D-30 replay transport),
# summary.json and the grid models. No Qt anywhere in this target.

wizz_collect_sources(WIZZ_UI_LOGIC_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(wizzairhelper_ui_logic STATIC ${WIZZ_UI_LOGIC_SOURCES})
add_library(wizz::ui_logic ALIAS wizzairhelper_ui_logic)

target_include_directories(wizzairhelper_ui_logic
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(wizzairhelper_ui_logic
    PUBLIC  wizz::core
            wizz::services)
