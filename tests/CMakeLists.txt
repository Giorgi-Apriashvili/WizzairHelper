# wizzairhelper_tests: offline conformance suite (05). Fake transport, fixed
# UTC clock, recording delay, fixture replay. Never touches live hosts.

find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

wizz_collect_sources(WIZZ_TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(wizzairhelper_tests ${WIZZ_TEST_SOURCES})

target_include_directories(wizzairhelper_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(wizzairhelper_tests PRIVATE
    wizz::core
    wizz::services
    GTest::gtest
    GTest::gtest_main)

# 05 §1.7: fixtures are read from <test-binary-dir>/Fixtures/<name>, byte for byte.
add_custom_command(TARGET wizzairhelper_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${PROJECT_SOURCE_DIR}/fixtures"
            "$<TARGET_FILE_DIR:wizzairhelper_tests>/Fixtures"
    COMMENT "Copying conformance fixtures")

gtest_discover_tests(wizzairhelper_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES ENVIRONMENT "WIZZ_TEST_FIXTURES=${PROJECT_SOURCE_DIR}/fixtures")
