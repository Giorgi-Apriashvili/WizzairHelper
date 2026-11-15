# wizzairhelper_gui_tests: widget-level tests (GoogleTest + a QApplication).
# Runs headless with the "minimal" Qt platform plugin.

find_package(Qt6 6.11 REQUIRED COMPONENTS Widgets)
include(WizzQtDeploy)

wizz_collect_sources(WIZZ_GUI_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(wizzairhelper_gui_tests ${WIZZ_GUI_TEST_SOURCES}
    # Fixture access (CAT, RATES, 05 §0.3) shared with the conformance suite.
    "${PROJECT_SOURCE_DIR}/tests/support/fixtures.cpp"
    "${PROJECT_SOURCE_DIR}/tests/support/fake_transport.cpp")
set_target_properties(wizzairhelper_gui_tests PROPERTIES AUTOMOC ON)

target_include_directories(wizzairhelper_gui_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/tests")

target_link_libraries(wizzairhelper_gui_tests PRIVATE
    wizz::app
    GTest::gtest)

# The --autotest replay scenarios (D-30), shared by the in-process flow tests.
target_compile_definitions(wizzairhelper_gui_tests PRIVATE
    WIZZ_TEST_REPLAY_DIR="${PROJECT_SOURCE_DIR}/tests/replay")

add_custom_command(TARGET wizzairhelper_gui_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${PROJECT_SOURCE_DIR}/fixtures"
            "$<TARGET_FILE_DIR:wizzairhelper_gui_tests>/Fixtures"
    VERBATIM)
wizz_copy_qt_plugins(wizzairhelper_gui_tests)

gtest_discover_tests(wizzairhelper_gui_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal")

# --autotest end to end (01 §17, 05 §5, D-30): the real WizzairHelper.exe on the
# replay scenarios of tests/replay (heavy_cached / cancel_cached generate theirs) with
# an isolated data root, the fixed clock and --diagnostics; check_autotest.py validates
# summary.json, diagnostics.json (40 ms timer gap below 100 ms — 250 ms in Debug —,
# UI-loop watchdog, one cancel transition, final grids, layout) and the screenshots.
# The window opens on the real "windows" platform (PrintWindow needs a native
# window), one at a time. Key scenarios repeat at 150 % and 200 % (QT_SCALE_FACTOR).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(_wizz_autotest_runs
        named_destination anywhere_return_from_top not_served same_area over_500
        cancel_waiting cancel_retrying heavy_cached cancel_cached
        named_destination@1.5 named_destination@2
        cancel_retrying@1.5 cancel_retrying@2
        heavy_cached@1.5 heavy_cached@2)
    foreach(run IN LISTS _wizz_autotest_runs)
        string(REPLACE "@" ";" _parts "${run}")
        list(GET _parts 0 scenario)
        set(_name autotest.${scenario})
        set(_scale_args)
        list(LENGTH _parts _count)
        if(_count GREATER 1)
            list(GET _parts 1 _scale)
            string(REPLACE "." "" _percent "${_scale}")
            if(_percent STREQUAL "2")
                set(_percent 200)
            else()
                set(_percent "${_percent}0")
            endif()
            set(_name autotest.${scenario}.scale${_percent})
            set(_scale_args --scale ${_scale})
        endif()
        add_test(NAME ${_name}
            COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/check_autotest.py"
                --exe "$<TARGET_FILE:WizzairHelper>"
                --replay-root "${PROJECT_SOURCE_DIR}/tests/replay"
                --scenario ${scenario}
                --max-gap-ms "$<IF:$<CONFIG:Debug>,250,100>"
                ${_scale_args}
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/autotest/$<CONFIG>/${_name}")
        set_tests_properties(${_name} PROPERTIES
            LABELS autotest
            RUN_SERIAL TRUE
            TIMEOUT 180)
    endforeach()
else()
    message(STATUS "Python 3 not found: the --autotest end-to-end checks (label autotest) are not registered")
endif()
