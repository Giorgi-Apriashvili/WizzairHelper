# wizzairhelper_gui_tests: widget-level tests (GoogleTest + a QApplication).
# Runs headless with the "minimal" Qt platform plugin.

find_package(Qt6 6.11 REQUIRED COMPONENTS Widgets)
include(WizzQtDeploy)

wizz_collect_sources(WIZZ_GUI_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(wizzairhelper_gui_tests ${WIZZ_GUI_TEST_SOURCES})
set_target_properties(wizzairhelper_gui_tests PROPERTIES AUTOMOC ON)

target_include_directories(wizzairhelper_gui_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/tests")

target_link_libraries(wizzairhelper_gui_tests PRIVATE
    wizz::app
    GTest::gtest)

add_custom_command(TARGET wizzairhelper_gui_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${PROJECT_SOURCE_DIR}/fixtures"
            "$<TARGET_FILE_DIR:wizzairhelper_gui_tests>/Fixtures"
    VERBATIM)
wizz_copy_qt_plugins(wizzairhelper_gui_tests)

gtest_discover_tests(wizzairhelper_gui_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal")
