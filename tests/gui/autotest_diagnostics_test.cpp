// diagnostics.json of --autotest --diagnostics (D-30, 05 §5): the final-grid check
// (late batches, rows of a previous search, duplicates), the layout merge over the
// captured states, the display / environment collectors and the document's keys.

#include "app_test_support.hpp"
#include "autotest_diagnostics.hpp"
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
    diagnostics.searches.push_back({.name = "outbound", .wizz_requests = 3, .duration_ms = 12.0, .final_status = "x"});
    diagnostics.grids.push_back({.name = "outbound", .rows = 3, .result_flights = 3});
    diagnostics.grids.push_back({.name = "return", .rows = 0, .result_flights = std::nullopt});
    diagnostics.layout = LayoutDiagnostics{.checks = 2, .problems = {}, .cancel_visible = true};
    const std::string json = wizz::app::to_json(diagnostics);

    std::size_t at = 0;
    for (const char* key : {"\"timer\"", "\"uiEvents\"", "\"cancel\"", "\"searches\"", "\"grids\"", "\"display\"",
                            "\"layout\"", "\"environment\""}) {
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
    const LayoutDiagnostics layout = wizz::app::describe_layout(rig.window());
    EXPECT_EQ(layout.checks, 1);
    EXPECT_TRUE(layout.cancel_visible);
    const wizz::app::EnvironmentDiagnostics environment = wizz::app::describe_environment(*wizz::Locale::en_us());
    EXPECT_EQ(environment.locale, "en-US");
    EXPECT_FALSE(environment.qt.empty());
    EXPECT_FALSE(environment.os.empty());
}

} // namespace
