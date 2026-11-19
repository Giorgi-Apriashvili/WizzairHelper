# wizzairhelper_gui_tests: widget-level tests (GoogleTest + a QApplication) under the app's
# own look (look_test_support.hpp). Runs headless with the "minimal" Qt platform plugin and
# its Windows font database (enable_fonts): real font metrics, so the layout tests measure the
# cards, buttons and headers as the app shows them (without it every glyph is a box as wide
# as the font is tall).

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

# The --autotest replay scenarios (D-30), shared by the in-process flow tests. The AppIcon
# tests (D-51) read the committed icon files and the icon group of the real WizzairHelper.exe
# of the same configuration, which is therefore built first.
target_compile_definitions(wizzairhelper_gui_tests PRIVATE
    WIZZ_TEST_REPLAY_DIR="${PROJECT_SOURCE_DIR}/tests/replay"
    WIZZ_TEST_ICON_DIR="${PROJECT_SOURCE_DIR}/src/app/resources/icon"
    WIZZ_TEST_APP_EXE="$<TARGET_FILE:WizzairHelper>")
add_dependencies(wizzairhelper_gui_tests WizzairHelper)

add_custom_command(TARGET wizzairhelper_gui_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${PROJECT_SOURCE_DIR}/fixtures"
            "$<TARGET_FILE_DIR:wizzairhelper_gui_tests>/Fixtures"
    VERBATIM)
wizz_copy_qt_plugins(wizzairhelper_gui_tests)

# The R-03 / 05 §5 timing tests (label timing) run alone: each checks the UI thread's own
# CPU time always and the wall-clock time unless other processes held the CPUs (DECISIONS
# D-50), so running them beside the rest of the suite would only leave the wall-clock half
# unchecked. ctest -L timing runs only these.
set(_wizz_timing_tests "FlightGridPerformance.*:ControllerFlows.R_03_*:ControllerFlows.The_progress_timer_*")
# Listing the tests starts the Qt application first (gui_test_main.cpp): about 0.3 s on a
# desktop, but a cold CI runner took more than CTest's default 5 s twice (DECISIONS D-50).
set(_wizz_gui_discovery_timeout 120)
gtest_discover_tests(wizzairhelper_gui_tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT ${_wizz_gui_discovery_timeout}
    TEST_FILTER "-${_wizz_timing_tests}"
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal:enable_fonts")
gtest_discover_tests(wizzairhelper_gui_tests
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT ${_wizz_gui_discovery_timeout}
    TEST_FILTER "${_wizz_timing_tests}"
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal:enable_fonts" LABELS timing RUN_SERIAL TRUE)

# The real WizzairHelper.exe --version (D-35): exactly "WizzairHelper <version>" and a line
# feed on standard output and exit code 0, read through pipes (app.version) and captured by
# Windows PowerShell inside a pipeline, as tools/release_check.ps1 does (app.version.powershell);
# the exe's version resource carries the same project version (app.version.resource). No window
# opens.
add_test(NAME app.version
    COMMAND "${CMAKE_COMMAND}" "-DEXE=$<TARGET_FILE:WizzairHelper>" "-DVERSION=${PROJECT_VERSION}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_version.cmake")
foreach(_check IN ITEMS powershell resource)
    set(_switch)
    if(_check STREQUAL "resource")
        set(_switch -Resource)
    endif()
    add_test(NAME app.version.${_check}
        COMMAND powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/check_version.ps1"
                -Exe "$<TARGET_FILE:WizzairHelper>" -Version "${PROJECT_VERSION}" ${_switch})
endforeach()
set_tests_properties(app.version app.version.powershell app.version.resource PROPERTIES TIMEOUT 60)

# --autotest end to end (01 §17, 05 §5, D-30): the real WizzairHelper.exe on the
# replay scenarios of tests/replay (heavy_cached / cancel_cached / restart_shifted generate
# theirs; restart_shifted runs the exe twice on one profile, the second run one day later,
# D-66, D-67) with an isolated data root, the fixed clock and --diagnostics; check_autotest.py validates
# summary.json, diagnostics.json (40 ms timer gap below 100 ms — 250 ms in Debug —,
# UI-loop watchdog, both by the UI thread's CPU time and by wall-clock time unless other
# processes held the CPUs (D-50); one cancel transition, final grids, layout; the look the
# run asked for, D-53..D-56) and the screenshots.
# The window opens on the real "windows" platform (PrintWindow needs a native
# window), one at a time. A run is "scenario[:variant]...", named
# autotest.<scenario>[.<variant>]...: scale150 / scale200 run at 150 % / 200 %
# (QT_SCALE_FACTOR), dark forces the dark look (WIZZ_COLOR_SCHEME=dark; the other runs
# follow the Windows app mode, and the checker verifies the look either way), minimum
# starts the window at the 980 x 600 minimum.
# The crash_* runs fail at start-up on purpose (a bad --fixed-now before the QApplication,
# a bad replay.json after it): DPI-aware crash box, errors.log record in the --data-root
# profile with the right timestamp (01 §3.2, D-30).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    set(_wizz_autotest_runs
        named_destination anywhere_return_from_top not_served same_area over_500
        cancel_waiting cancel_retrying heavy_cached cancel_cached restart_shifted
        crash_bad_fixed_now crash_bad_replay
        named_destination:scale150 named_destination:scale200
        cancel_retrying:scale150 cancel_retrying:scale200
        heavy_cached:scale150 heavy_cached:scale200
        named_destination:minimum
        named_destination:dark cancel_retrying:dark heavy_cached:dark
        named_destination:dark:scale200 named_destination:dark:minimum)
    foreach(run IN LISTS _wizz_autotest_runs)
        string(REPLACE ":" ";" _variants "${run}")
        list(POP_FRONT _variants scenario)
        set(_name autotest.${scenario})
        set(_variant_args)
        foreach(_variant IN LISTS _variants)
            string(APPEND _name ".${_variant}")
            if(_variant STREQUAL "dark")
                list(APPEND _variant_args --color-scheme dark)
            elseif(_variant STREQUAL "minimum")
                list(APPEND _variant_args --window 980x600)
            elseif(_variant MATCHES "^scale([1-9][0-9]*)$")
                math(EXPR _whole "${CMAKE_MATCH_1} / 100")
                math(EXPR _tenths "(${CMAKE_MATCH_1} % 100) / 10")
                if(_tenths EQUAL 0)
                    list(APPEND _variant_args --scale ${_whole})
                else()
                    list(APPEND _variant_args --scale ${_whole}.${_tenths})
                endif()
            else()
                message(FATAL_ERROR "autotest run ${run}: unknown variant ${_variant}")
            endif()
        endforeach()
        add_test(NAME ${_name}
            COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/check_autotest.py"
                --exe "$<TARGET_FILE:WizzairHelper>"
                --replay-root "${PROJECT_SOURCE_DIR}/tests/replay"
                --scenario ${scenario}
                --max-gap-ms "$<IF:$<CONFIG:Debug>,250,100>"
                ${_variant_args}
                --work-dir "${CMAKE_CURRENT_BINARY_DIR}/autotest/$<CONFIG>/${_name}")
        set_tests_properties(${_name} PROPERTIES
            LABELS autotest
            RUN_SERIAL TRUE
            TIMEOUT 180)
    endforeach()
else()
    message(STATUS "Python 3 not found: the --autotest end-to-end checks (label autotest) are not registered")
endif()
