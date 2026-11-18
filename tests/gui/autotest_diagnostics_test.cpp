// diagnostics.json of --autotest --diagnostics (D-30, 05 §5): the final-grid check
// (late batches, rows of a previous search, duplicates), the layout merge over the
// captured states, the start-up / display / environment collectors and the document's
// keys.

#include "app_test_support.hpp"
#include "autotest_diagnostics.hpp"
#include "look_test_support.hpp"
#include "ui_style.hpp"
#include "ui_logic/ui_samples.hpp"

#include "wizz/core/json.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::FlightResult;
using wizz::FlightResultPtr;
using wizz::LegSearchResult;
using wizz::app::AutoTestDiagnostics;
using wizz::app::describe_grid;
using wizz::app::GridDiagnostics;
using wizz::app::LayoutDiagnostics;
using wizz::test::app::WindowRig;

[[nodiscard]] LegSearchResult result_of(std::vector<FlightResultPtr> flights)
{
    LegSearchResult result;
    result.flights = std::move(flights);
    return result;
}

TEST(AutoTestDiagnostics, A_final_grid_equal_to_its_result_is_clean)
{
    const std::vector<FlightResultPtr> flights = wizz::test::samples::section11_flights();
    const LegSearchResult result = result_of(flights);
    const GridDiagnostics grid = describe_grid("outbound", flights, &result);
    EXPECT_EQ(grid.name, "outbound");
    EXPECT_EQ(grid.rows, 12u);
    EXPECT_EQ(grid.result_flights, 12u);
    EXPECT_EQ(grid.rows_not_in_result, 0u);
    EXPECT_EQ(grid.duplicate_rows, 0u);
}

TEST(AutoTestDiagnostics, Late_rows_and_rows_of_a_previous_search_are_counted)
{
    const std::vector<FlightResultPtr> flights = wizz::test::samples::section11_flights();
    const LegSearchResult result = result_of(flights);
    std::vector<FlightResultPtr> rows = flights;
    rows.push_back(std::make_shared<const FlightResult>(*flights.front()));  // same fare, another search
    rows.push_back(flights.back());                                          // a batch appended twice
    const GridDiagnostics grid = describe_grid("outbound", rows, &result);
    EXPECT_EQ(grid.rows, 14u);
    EXPECT_EQ(grid.rows_not_in_result, 1u);
    EXPECT_EQ(grid.duplicate_rows, 2u);

    const GridDiagnostics no_result = describe_grid("return", rows, nullptr);
    EXPECT_FALSE(no_result.result_flights.has_value());
    EXPECT_EQ(no_result.rows_not_in_result, 14u);
}

TEST(AutoTestDiagnostics, Layout_checks_merge_over_the_captured_states)
{
    LayoutDiagnostics merged;
    merged.merge("1-outbound.png", LayoutDiagnostics{.checks = 1, .problems = {}, .cancel_visible = true});
    EXPECT_EQ(merged.checks, 1);
    EXPECT_TRUE(merged.cancel_visible);
    merged.merge("progress-1.png",
                 LayoutDiagnostics{.checks = 1, .problems = {"QLabel 'x': cut off by the window"}, .cancel_visible = false});
    merged.merge("4-log.png", LayoutDiagnostics{.checks = 1, .problems = {}, .cancel_visible = true});
    EXPECT_EQ(merged.checks, 3);
    EXPECT_FALSE(merged.cancel_visible) << "Cancel must be visible in every state";
    EXPECT_EQ(merged.problems, std::vector<std::string>{"progress-1.png: QLabel 'x': cut off by the window"});
}

TEST(AutoTestDiagnostics, The_document_has_the_documented_keys_in_order)
{
    AutoTestDiagnostics diagnostics;
    diagnostics.timer.runs = 2;
    diagnostics.timer.ticks = 10;
    diagnostics.timer.longest_gap_ms = 41.26;
    diagnostics.timer.longest_gaps.push_back({.ms = 41.26, .ended_at_ms = 900, .status = "Fetching fares"});
    diagnostics.cancel = wizz::app::CancelDiagnostics{.after = 1500ms,
                                                      .pressed_at_ms = 1500.04,
                                                      .busy_at_press = true,
                                                      .status_before = "Fetching fares",
                                                      .presses = {"Esc", "Cancel button"},
                                                      .cancel_requests = 1,
                                                      .status_after_press = "Cancelling…",
                                                      .cancel_enabled_after_press = false};
    diagnostics.searches.push_back(
        {.name = "outbound", .wizz_requests = 3, .duration_ms = 12.0, .final_status = "x", .final_status_cut = true});
    diagnostics.grids.push_back({.name = "outbound", .rows = 3, .result_flights = 3});
    diagnostics.grids.push_back({.name = "return", .rows = 0, .result_flights = std::nullopt});
    diagnostics.layout = LayoutDiagnostics{.checks = 2, .problems = {}, .cancel_visible = true};
    diagnostics.startup = wizz::app::StartupDiagnostics{
        .window_animates = true,
        .settings = {.client_area_animation = true, .minimize_animation = true, .policy_disallows = false},
        .start_delay_ms = 300,
        .work_started_after_show_ms = 300.04,
        .first_busy_after_show_ms = std::nullopt};
    const std::string json = wizz::app::to_json(diagnostics);

    std::size_t at = 0;
    for (const char* key : {"\"timer\"", "\"uiEvents\"", "\"startup\"", "\"cancel\"", "\"searches\"",
                            "\"grids\"", "\"display\"", "\"layout\"", "\"environment\""}) {
        const std::size_t found = json.find(key, at);
        ASSERT_NE(found, std::string::npos) << key;
        at = found;
    }
    EXPECT_TRUE(json.contains("\"longestGapMs\": 41.3")) << json.substr(0, 300);
    EXPECT_TRUE(json.contains("\"uiEvents\": null"));
    EXPECT_TRUE(json.contains("\"afterMs\": 1500"));
    EXPECT_TRUE(json.contains("\"cancelRequests\": 1"));
    EXPECT_TRUE(json.contains("\"resultFlights\": null"));
    EXPECT_TRUE(json.contains("\"statesChecked\": 2"));
    EXPECT_TRUE(json.contains("\"finalStatusCut\": true"));
    EXPECT_TRUE(json.contains("\"windowAnimates\": true"));
    EXPECT_TRUE(json.contains("\"clientAreaAnimation\": true"));
    EXPECT_TRUE(json.contains("\"minimizeAnimation\": true"));
    EXPECT_TRUE(json.contains("\"policyDisallowsAnimations\": false"));
    EXPECT_TRUE(json.contains("\"startDelayMs\": 300"));
    EXPECT_TRUE(json.contains("\"workStartedAfterShowMs\": 300"));
    EXPECT_TRUE(json.contains("\"firstBusyAfterShowMs\": null"));
    const wizz::json::Value parsed = wizz::json::parse(json);
    EXPECT_TRUE(parsed.is_object());
}

TEST(AutoTestDiagnostics, Display_layout_and_environment_of_the_main_window)
{
    WindowRig rig;
    rig.start();
    const wizz::app::DisplayDiagnostics display = wizz::app::describe_display(rig.window());
    EXPECT_FALSE(display.platform.empty());
    EXPECT_GT(display.device_pixel_ratio, 0.0);
    EXPECT_FALSE(display.window_client.isEmpty());
    EXPECT_GT(display.outer_device_size.width(), 0);
    EXPECT_EQ(display.minimum_window, wizz::app::MainWindow::kMinimumSize);
    EXPECT_EQ(display.default_window, wizz::app::MainWindow::kDefaultSize);
    // The start-up (D-65): the "minimal" platform never animates, so the map load began at once.
    const wizz::app::StartupDiagnostics startup = wizz::app::describe_startup(rig.window());
    EXPECT_EQ(startup.window_animates, rig.window().startup_record().window_animates);
    EXPECT_EQ(startup.start_delay_ms, startup.window_animates ? 300.0 : 0.0);
    ASSERT_TRUE(startup.work_started_after_show_ms.has_value());
    ASSERT_TRUE(startup.first_busy_after_show_ms.has_value());
    EXPECT_GE(*startup.work_started_after_show_ms, startup.start_delay_ms);
    EXPECT_GE(*startup.first_busy_after_show_ms, *startup.work_started_after_show_ms);
    const LayoutDiagnostics layout = wizz::app::describe_layout(rig.window());
    EXPECT_EQ(layout.checks, 1);
    EXPECT_TRUE(layout.cancel_visible);
    const wizz::app::EnvironmentDiagnostics environment = wizz::app::describe_environment(*wizz::Locale::en_us());
    EXPECT_EQ(environment.locale, "en-US");
    EXPECT_FALSE(environment.qt.empty());
    EXPECT_FALSE(environment.os.empty());
    // The look in use (the suite runs the app's light look): what a dark run checks.
    EXPECT_EQ(environment.style, "modern/fusion");
    EXPECT_EQ(environment.look, wizz::app::style::theme().dark ? "dark" : "light");
    EXPECT_EQ(environment.window_colour, wizz::app::style::theme().window.name().toStdString());
    EXPECT_FALSE(environment.font_family.empty());
}

TEST(AutoTestDiagnostics, The_environment_names_a_dark_and_a_high_contrast_look)
{
    const wizz::test::look::LookGuard guard;
    qputenv("WIZZ_COLOR_SCHEME", "dark");
    ASSERT_TRUE(wizz::app::style::apply_application_look());
    wizz::app::EnvironmentDiagnostics environment = wizz::app::describe_environment(*wizz::Locale::en_us());
    EXPECT_EQ(environment.look, "dark");
    EXPECT_EQ(environment.window_colour, "#131217");
    EXPECT_EQ(environment.style, "modern/fusion");

    wizz::app::style::set_high_contrast_probe([] { return true; });
    ASSERT_TRUE(wizz::app::style::refresh_appearance());
    environment = wizz::app::describe_environment(*wizz::Locale::en_us());
    EXPECT_EQ(environment.look, "high_contrast");
    EXPECT_TRUE(environment.style == "windowsvista" || environment.style == "windows") << environment.style;

    AutoTestDiagnostics diagnostics;
    diagnostics.environment = environment;
    const std::string json = wizz::app::to_json(diagnostics);
    EXPECT_TRUE(json.contains("\"look\": \"high_contrast\"")) << json;
    EXPECT_TRUE(json.contains("\"windowColour\": \"#")) << json;
}

} // namespace
