// The MainForm flows of 01 §11 through the real window, controller, services and
// a replayed network (tests/replay scenarios, D-30): LoadCatalogAsync,
// PrepareAsync / ResolveAsync with the chooser, RunLegAsync (streaming, the
// 0-request path, the >500 cap, the >60 question, exceptions), the right-click
// return, the currency switch without requests, RecomputeTrips, tab titles and
// every Log line of §12.2 these paths write.

#include "app_test_support.hpp"

#include "date_picker.hpp"
#include "place_combo.hpp"
#include "qt_strings.hpp"

#include "support/fixtures.hpp"

#include "wizz/core/http.hpp"
#include "wizz/ui/date_rules.hpp"
#include "wizz/ui/leg_text.hpp"
#include "wizz/ui/strings.hpp"

#include <QCheckBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QMenu>
#include <QTabWidget>
#include <QTimer>

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::Date;
using wizz::app::BusyPanel;
using wizz::app::MainWindow;
using wizz::test::app::json_rule;
using wizz::test::app::scenario_rules;
using wizz::test::app::WindowRig;
using wizz::test::app::WindowRigOptions;
namespace strings = wizz::ui::strings;
namespace gui = wizz::test::gui;

constexpr Date d(int year, unsigned month, unsigned day)
{
    return Date::from_ymd(year, month, day);
}

/// The 03 §11 request: Budapest r200 → London r30, 10–20 Nov, return 12–20 Nov.
void section11_inputs(WindowRig& rig)
{
    rig.input().set_from_radius_km(200);
    rig.input().set_to_radius_km(30);
    rig.input().apply_autotest_dates(wizz::ui::DateWindow{.start = d(2026, 11, 10), .end = d(2026, 11, 20)},
                                     wizz::ui::DateWindow{.start = d(2026, 11, 12), .end = d(2026, 11, 20)});
}

[[nodiscard]] std::string text(const QString& value)
{
    return value.toStdString();
}

TEST(ControllerFlows, Named_destination_outbound_streams_into_the_grid_and_ends_green)
{
    WindowRig rig;
    rig.start();
    section11_inputs(rig);
    int contents_changes = 0;
    QObject::connect(&rig.outbound(), &wizz::app::FlightGrid::contents_changed, [&] { ++contents_changes; });
    rig.window().select_page(MainWindow::Page::log);

    rig.run(rig.controller().search_outbound());

    EXPECT_EQ(rig.outbound().row_count(), 12u);
    EXPECT_EQ(rig.outbound().all_results().size(), 12u);
    EXPECT_EQ(rig.tab_text(MainWindow::Page::outbound), "Outbound (12)");
    EXPECT_EQ(rig.window().tabs()->currentWidget(), &rig.outbound()) << "the leg's tab is selected";
    EXPECT_GE(contents_changes, 2) << "Clear, (flushes,) SetResults";
    EXPECT_EQ(rig.outbound().summary_text(),
              "12 fares \u00B7 cheapest 14.99 EUR: BTS \u2192 LTN on Thu 12 Nov");
    EXPECT_EQ(rig.busy().status_text(), "Outbound Done: 12 fares from 3 of 3 requests in 1 s.");
    EXPECT_EQ(rig.busy().status_colour(), BusyPanel::kSuccessColour);
    EXPECT_EQ(text(rig.input().resolved_text()),
              "Outbound: from Budapest (BUD), Hungary \u2192 airports within 200 km: BUD 0 km, BTS 173 km, DEB 184 km.   "
              "To London, United Kingdom \u2014 Wizz Air airports: LGW, LTN \u2192 airports within 30 km: LGW 41 km, "
              "LTN 44 km.");
    EXPECT_FALSE(rig.controller().is_working());
    ASSERT_NE(rig.controller().outbound_result(), nullptr);
    EXPECT_EQ(rig.controller().outbound_result()->request.label, "Outbound");
    EXPECT_EQ(rig.outbound().current_row_text(), "2026-11-12 BTS->LTN 14.99");
    EXPECT_EQ(rig.wizz_requests(), 5) << "homepage, map, 3 timetables";
    EXPECT_EQ(rig.trips().summary_text(), strings::kTripsPlaceholder);

    // LogPlan and LogResult (01 §12.2).
    for (const char* line :
         {"Outbound: Budapest (BUD), Hungary (+200 km) \u2192 London, United Kingdom \u2014 Wizz Air airports: LGW, LTN "
          "(+30 km), 2026-11-10 \u2026 2026-11-20, 1 adult(s), Regular, EUR",
          "  origin airports: BUD (0 km), BTS (173 km), DEB (184 km)",
          "  destination airports: LGW (41 km), LTN (44 km)", "  3 route(s), 3 request(s)",
          "Outbound: 12 fares, 3/3 requests, 1 route(s) not served, 1 s, FX Frankfurter 2026-11-02",
          "    Thu 12 Nov 07:15  BTS\u2192LTN  14.99 EUR  = 14.99 EUR"}) {
        EXPECT_TRUE(rig.log_contains(line)) << line;
    }

    // The booking link of the current row (CT-LNK-01).
    rig.outbound().open_selected();
    ASSERT_EQ(rig.actions.opened.size(), 1u);
    EXPECT_EQ(rig.actions.opened[0], "https://www.wizzair.com/en-gb/booking/select-flight/BTS/LTN/2026-11-12/null/1/0/0/null");
}

TEST(ControllerFlows, Return_round_trips_and_a_currency_switch_without_requests)
{
    WindowRig rig;
    rig.start();
    section11_inputs(rig);
    rig.run(rig.controller().search_outbound());
    rig.run(rig.controller().search_return());

    EXPECT_EQ(rig.inbound().row_count(), 4u);
    EXPECT_EQ(rig.tab_text(MainWindow::Page::inbound), "Return (4)");
    EXPECT_EQ(rig.tab_text(MainWindow::Page::round_trips), "Round trips (22)");
    EXPECT_EQ(rig.trips().count(), 22u);
    EXPECT_EQ(rig.trips().summary_text(),
              "22 combinations \u00B7 cheapest 36.53 EUR: BTS\u2192LTN Thu 12 Nov, back LTN\u2192BUD Tue 17 Nov (5 nights)");
    EXPECT_EQ(rig.busy().status_text(), "Return Done: 4 fares from 4 of 4 requests in 1 s.");
    EXPECT_EQ(rig.wizz_requests(), 9);

    // Two one-way bookings for the cheapest trip (not the same home airport).
    rig.trips().open_selected();
    ASSERT_EQ(rig.actions.opened.size(), 2u);
    EXPECT_EQ(rig.actions.opened[0], "https://www.wizzair.com/en-gb/booking/select-flight/BTS/LTN/2026-11-12/null/1/0/0/null");
    EXPECT_EQ(rig.actions.opened[1], "https://www.wizzair.com/en-gb/booking/select-flight/LTN/BUD/2026-11-17/null/1/0/0/null");

    // §11.7: re-pricing uses the stored rates; nothing goes to the network.
    const std::size_t http_before = rig.replay().request_count();
    const std::size_t log_before = rig.log_lines().size();
    const std::string status_before = rig.busy().status_text();
    ASSERT_TRUE(rig.input().set_currency("HUF"));
    EXPECT_EQ(rig.replay().request_count(), http_before) << "repricing must issue zero network requests (05 \u00A75)";
    EXPECT_EQ(rig.wizz_requests(), 9);
    EXPECT_EQ(rig.outbound().summary_text(), "12 fares \u00B7 cheapest 5,491 HUF: BTS \u2192 LTN on Thu 12 Nov");
    EXPECT_EQ(rig.outbound().current_row_text(), "2026-11-12 BTS->LTN 5491");
    EXPECT_EQ(rig.trips().summary_text(),
              "22 combinations \u00B7 cheapest 13,379 HUF: BTS\u2192LTN Thu 12 Nov, back LTN\u2192BUD Tue 17 Nov (5 nights)");
    EXPECT_EQ(rig.controller().outbound_result()->request.currency, "HUF");
    EXPECT_EQ(rig.controller().return_result()->request.currency, "HUF");
    EXPECT_EQ(rig.busy().status_text(), status_before) << "the busy line is not updated";
    EXPECT_EQ(rig.log_lines().size(), log_before) << "nothing is logged";
    EXPECT_EQ(rig.tab_text(MainWindow::Page::outbound), "Outbound (12)");
}

TEST(ControllerFlows, A_return_without_destination_or_pick_fails_with_the_type_text)
{
    WindowRig rig;
    rig.start();
    rig.input().set_to_text("");
    rig.run(rig.controller().search_return());

    const std::string expected{strings::kReturnNeedsDestination};
    EXPECT_EQ(rig.busy().status_text(), expected);
    EXPECT_EQ(rig.busy().status_colour(), BusyPanel::kFailureColour);
    EXPECT_TRUE(rig.log_contains(expected));
    ASSERT_EQ(rig.dialogs.errors.size(), 1u);
    EXPECT_EQ(text(rig.dialogs.errors[0]), expected);
    EXPECT_EQ(rig.wizz_requests(), 2) << "no timetable request";
    EXPECT_FALSE(rig.controller().is_working());
    EXPECT_FALSE(rig.controller().has_active_phase());
}

TEST(ControllerFlows, Anywhere_then_return_needs_a_pick_then_the_right_click_return_runs)
{
    WindowRigOptions options;
    options.scenario = "anywhere_return_from_top";
    WindowRig rig{std::move(options)};
    rig.start();
    rig.input().set_from_radius_km(0);
    rig.input().set_to_text("");
    rig.input().apply_autotest_dates(wizz::ui::DateWindow{.start = d(2026, 11, 10), .end = d(2026, 12, 10)},
                                     std::nullopt);
    rig.run(rig.controller().search_outbound());
    EXPECT_EQ(rig.outbound().mode(), wizz::ui::ViewMode::cheapest_per_destination) << "LIVE-5 auto view";
    EXPECT_EQ(rig.outbound().all_results().size(), 21u);
    EXPECT_EQ(rig.busy().status_text(),
              "Outbound Done: 21 fares from 5 of 5 requests in 1 s. 1 note(s) in the Log tab.");
    EXPECT_TRUE(rig.log_contains("  note: 1 route(s) skipped because their next flight is after your dates: BUD "
                                 "\u2192 Alghero (Sardinia): next flight 07 Jun 2027."));

    // "To" empty and no pick yet: the outbound holds fares, so the text asks for a pick.
    rig.run(rig.controller().search_return());
    EXPECT_EQ(rig.busy().status_text(), strings::kReturnNeedsPick);
    ASSERT_EQ(rig.dialogs.errors.size(), 1u);

    // The context menu offers the right-click return only when idle (UI-9).
    QMenu* menu = rig.outbound().prepare_context_menu();
    ASSERT_NE(menu, nullptr);
    const wizz::FlightResultPtr top = rig.outbound().selected();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->to_airport->iata, "NAP");
    rig.run(rig.controller().plan_return_from(top));

    EXPECT_TRUE(rig.log_contains(
        "Return will start from Naples (NAP) (picked in the outbound results), 03 Dec \u2013 11 Dec."));
    EXPECT_EQ(rig.input().ret_from(), d(2026, 12, 3));
    EXPECT_EQ(rig.input().ret_to(), d(2026, 12, 11));
    EXPECT_TRUE(text(rig.input().resolved_text())
                    .starts_with("Return: from Naples (NAP), Italy \u2192 airports within 50 km: NAP 0 km."))
        << text(rig.input().resolved_text());
    ASSERT_NE(rig.controller().return_result(), nullptr);
    EXPECT_EQ(rig.controller().return_result()->request.origin_radius_km, 50.0) << "min(To radius, 50)";
    EXPECT_EQ(rig.inbound().row_count(), 3u);
    EXPECT_EQ(rig.trips().count(), 13u);
    EXPECT_EQ(rig.window().tabs()->currentWidget(), &rig.inbound());
}

TEST(ControllerFlows, Same_area_takes_the_zero_request_path_with_the_first_note)
{
    WindowRigOptions options;
    options.scenario = "same_area";
    WindowRig rig{std::move(options)};
    rig.start();
    rig.input().set_to_text("BUD");
    rig.window().select_page(MainWindow::Page::log);
    rig.run(rig.controller().search_outbound());

    const std::string note =
        "Start and destination are the same area (0 km apart). Choose a destination farther away, or leave "
        "\u201CTo\u201D empty to search anywhere.";
    EXPECT_EQ(rig.busy().status_text(), note);
    EXPECT_EQ(rig.busy().status_colour(), BusyPanel::kFailureColour);
    EXPECT_EQ(rig.outbound().summary_text(), note);
    EXPECT_EQ(rig.window().tabs()->currentWidget(), &rig.outbound());
    EXPECT_EQ(rig.tab_text(MainWindow::Page::outbound), "Outbound");
    EXPECT_EQ(rig.controller().outbound_result(), nullptr);
    EXPECT_TRUE(text(rig.input().resolved_text()).contains(strings::kResolvedSameArea)) << "U-11";
    EXPECT_TRUE(rig.log_contains(strings::kSameAreaLogLine));
    EXPECT_EQ(rig.wizz_requests(), 2);
    EXPECT_TRUE(rig.dialogs.errors.empty());
}

TEST(ControllerFlows, Not_served_routes_end_with_the_route_map_summary)
{
    WindowRigOptions options;
    options.scenario = "not_served";
    WindowRig rig{std::move(options)};
    rig.start();
    rig.input().set_from_text("Debrecen");
    rig.input().set_from_radius_km(0);
    rig.input().set_to_radius_km(30);
    rig.run(rig.controller().search_outbound());
    EXPECT_EQ(rig.outbound().summary_text(), strings::kSummaryRoutesNotServed);
    EXPECT_EQ(rig.busy().status_text(), "Outbound Done: 0 fares from 1 of 1 requests in 1 s.");
    EXPECT_EQ(rig.wizz_requests(), 3);
}

TEST(ControllerFlows, More_than_500_requests_are_refused_and_the_old_results_stay)
{
    std::vector<wizz::ui::ReplayTransport::Rule> rules = scenario_rules("over_500");
    WindowRigOptions options;
    options.rules = std::move(rules);
    WindowRig rig{std::move(options)};
    rig.start();
    rig.input().set_from_radius_km(1500);
    rig.input().set_to_text("");
    rig.input().apply_autotest_dates(wizz::ui::DateWindow{.start = d(2026, 11, 3), .end = d(2028, 11, 1)},
                                     std::nullopt);
    rig.outbound().set_summary("previous results");
    rig.run(rig.controller().search_outbound());

    const std::string cap = wizz::ui::request_cap_text(6794);
    EXPECT_EQ(cap, "This search would need 6794 requests to Wizz Air (about 148 min). That is more than the 500 this "
                   "app allows, to stay polite to the site. Use a smaller radius, fewer dates, or a specific "
                   "destination.");
    EXPECT_EQ(rig.busy().status_text(), cap);
    EXPECT_EQ(rig.busy().status_colour(), BusyPanel::kFailureColour);
    EXPECT_TRUE(rig.log_contains(cap));
    ASSERT_EQ(rig.dialogs.errors.size(), 1u);
    EXPECT_EQ(text(rig.dialogs.errors[0]), cap);
    EXPECT_EQ(rig.outbound().summary_text(), "previous results") << "[WART] the grid is left alone";
    EXPECT_EQ(rig.wizz_requests(), 2);
    EXPECT_TRUE(rig.dialogs.questions.empty());
}

TEST(ControllerFlows, A_big_search_asks_first_and_No_does_not_start_it)
{
    WindowRig rig;
    rig.start();
    rig.input().set_from_radius_km(200);
    rig.input().set_to_text("");
    rig.input().apply_autotest_dates(wizz::ui::DateWindow{.start = d(2026, 11, 3), .end = d(2027, 11, 1)},
                                     std::nullopt);
    rig.dialogs.answer_big_search = false;
    rig.run(rig.controller().search_outbound());

    ASSERT_EQ(rig.dialogs.questions.size(), 1u);
    const std::string question = text(rig.dialogs.questions[0]);
    EXPECT_TRUE(question.starts_with("This search needs ")) << question;
    EXPECT_TRUE(question.ends_with("Tip: a smaller radius or a specific destination makes it much faster.\n\n"
                                   "Continue?"))
        << question;
    EXPECT_EQ(rig.busy().status_text(), strings::kSearchNotStarted);
    EXPECT_EQ(rig.busy().status_colour(), BusyPanel::kSuccessColour);
    EXPECT_EQ(rig.wizz_requests(), 2);
    EXPECT_FALSE(rig.controller().is_working());
}

TEST(ControllerFlows, An_empty_From_shows_the_box_and_focuses_From)
{
    WindowRig rig;
    rig.start();
    const std::string before = rig.busy().status_text();
    rig.input().set_from_text("   ");
    rig.input().ret_to_picker()->setFocus();
    rig.run(rig.controller().search_outbound());
    ASSERT_EQ(rig.dialogs.errors.size(), 1u);
    EXPECT_EQ(text(rig.dialogs.errors[0]), strings::kTypeStart);
    EXPECT_EQ(rig.window().focusWidget(), rig.input().from_box()) << "the editable combo takes the focus of its line edit";
    EXPECT_EQ(rig.busy().status_text(), before) << "no busy phase starts";
    EXPECT_EQ(rig.wizz_requests(), 2);
}

TEST(ControllerFlows, A_silent_typo_correction_is_logged)
{
    std::vector<wizz::ui::ReplayTransport::Rule> rules = scenario_rules("named_destination");
    rules.insert(rules.begin(), json_rule("photon.komoot.io", wizz::test::read_fixture("geo_photon_london.json")));
    WindowRigOptions options;
    options.rules = std::move(rules);
    WindowRig rig{std::move(options)};
    rig.start();
    section11_inputs(rig);
    rig.input().set_to_text("Londn");
    rig.run(rig.controller().search_outbound());
    EXPECT_TRUE(rig.dialogs.chooser_calls.empty());
    EXPECT_TRUE(rig.log_contains("Interpreted \u201CLondn\u201D as \u201CLondon\u201D."));
    EXPECT_EQ(rig.outbound().all_results().size(), 12u);
}

/// PH-ONT (05 §2): the geocoder only knows London, Ontario.
constexpr const char* kPhotonOntario =
    R"({"type":"FeatureCollection","features":[{"type":"Feature","properties":{"osm_value":"city","name":"London",)"
    R"("state":"Ontario","country":"Canada"},"geometry":{"type":"Point","coordinates":[-81.2453,42.9849]}}]})";

TEST(ControllerFlows, The_chooser_waits_for_the_user_and_Cancel_does_not_start_the_search)
{
    std::vector<wizz::ui::ReplayTransport::Rule> rules = scenario_rules("named_destination");
    rules.insert(rules.begin(), json_rule("photon.komoot.io", kPhotonOntario));
    WindowRigOptions options;
    options.rules = std::move(rules);
    WindowRig rig{std::move(options)};
    rig.start();
    section11_inputs(rig);
    rig.input().set_to_text("Londn");

    rig.dialogs.chooser_answer = std::nullopt;  // Cancel (CT-GEOC-04)
    rig.run(rig.controller().search_outbound());
    ASSERT_EQ(rig.dialogs.chooser_calls.size(), 1u);
    const auto& call = rig.dialogs.chooser_calls[0];
    EXPECT_EQ(call.typed, "Londn");
    EXPECT_EQ(call.role, strings::kRoleDestination);
    ASSERT_EQ(call.candidates.size(), 2u);
    EXPECT_EQ(call.candidates[0],
              "London, United Kingdom \u2014 Wizz Air airports: LGW, LTN (from \u201CLondn\u201D)");
    EXPECT_EQ(call.candidates[1], "London, Ontario, Canada");
    EXPECT_EQ(call.busy_status, "Waiting for your choice:  which place is \u201CLondn\u201D?");
    EXPECT_TRUE(call.busy) << "the panel keeps animating behind the modal chooser";
    EXPECT_EQ(rig.busy().status_text(), strings::kSearchNotStarted);
    EXPECT_EQ(rig.busy().status_colour(), BusyPanel::kSuccessColour) << "UI-10: green, no box";
    EXPECT_TRUE(rig.dialogs.errors.empty());
    EXPECT_FALSE(rig.log_contains("Interpreted \u201CLondn\u201D as \u201CLondon\u201D."));
    EXPECT_EQ(rig.wizz_requests(), 2);

    rig.dialogs.chooser_answer = 0;  // "Use this place" on the corrected Wizz city
    rig.run(rig.controller().search_outbound());
    EXPECT_EQ(rig.dialogs.chooser_calls.size(), 2u) << "the answer is not remembered";
    EXPECT_TRUE(rig.log_contains("Interpreted \u201CLondn\u201D as \u201CLondon\u201D."));
    EXPECT_EQ(rig.outbound().all_results().size(), 12u);
}

TEST(ControllerFlows, In_autotest_the_first_candidate_is_used_and_no_box_is_shown)
{
    std::vector<wizz::ui::ReplayTransport::Rule> rules = scenario_rules("named_destination");
    rules.insert(rules.begin(), json_rule("photon.komoot.io", kPhotonOntario));
    WindowRigOptions options;
    options.rules = std::move(rules);
    options.autotest = wizz::ui::AutoTestOptions{};
    options.autotest->keep_open = true;
    WindowRig rig{std::move(options)};
    ASSERT_NE(rig.window().autotest_runner(), nullptr);
    // Drive the flows directly; the script itself is covered by the AutoTestRunner tests.
    rig.run(rig.controller().load_catalog());
    section11_inputs(rig);
    rig.input().set_to_text("Londn");
    rig.input().set_to_radius_km(30);
    rig.run(rig.controller().search_outbound());
    EXPECT_TRUE(rig.dialogs.chooser_calls.empty());
    EXPECT_TRUE(rig.log_contains("Interpreted \u201CLondn\u201D as \u201CLondon\u201D."));
    EXPECT_EQ(rig.outbound().all_results().size(), 12u);

    rig.input().set_to_text("");
    rig.run(rig.controller().search_return());
    EXPECT_EQ(rig.busy().status_text(), strings::kReturnNeedsPick);
    EXPECT_TRUE(rig.dialogs.errors.empty()) << "Fail boxes are suppressed in autotest";
}

TEST(ControllerFlows, A_failed_route_map_load_is_red_and_retried_on_the_next_search)
{
    std::vector<wizz::ui::ReplayTransport::Rule> rules;
    {
        wizz::ui::ReplayTransport::Rule home;
        home.url_prefix = "https://www.wizzair.com";
        home.body = std::string{wizz::test::kHomeHtml};
        home.content_type = "text/html; charset=utf-8";
        rules.push_back(home);
        rules.push_back(json_rule("/asset/map", R"({"x":1})"));
    }
    WindowRigOptions options;
    options.rules = std::move(rules);
    WindowRig rig{std::move(options)};
    rig.start();
    EXPECT_EQ(rig.controller().catalog(), nullptr);
    EXPECT_TRUE(rig.busy().status_text().starts_with("Could not load the Wizz Air route map: "))
        << rig.busy().status_text();
    EXPECT_EQ(rig.busy().status_colour(), BusyPanel::kFailureColour);
    const std::vector<std::string> log = rig.log_lines();
    ASSERT_EQ(log.size(), 2u);
    EXPECT_TRUE(log[1].starts_with("ERROR loading route map: ")) << log[1];
    EXPECT_TRUE(rig.dialogs.errors.empty()) << "load failures show no MessageBox";
    const std::int64_t requests = rig.wizz_requests();

    rig.run(rig.controller().search_outbound());
    EXPECT_GT(rig.wizz_requests(), requests) << "the map is loaded again when a Search button is pressed";
    EXPECT_TRUE(rig.busy().status_text().starts_with("Could not load the Wizz Air route map: "));
    EXPECT_FALSE(rig.controller().is_working());
}

/// Throws an unexpected exception type for timetable requests.
class ExplodingTransport final : public wizz::http::Transport {
public:
    explicit ExplodingTransport(std::shared_ptr<wizz::http::Transport> inner) : inner_(std::move(inner)) {}
    wizz::http::Response send(const wizz::http::Request& request, std::stop_token stop) override
    {
        if (request.url.contains("/search/timetableV2")) {
            throw std::logic_error("unexpected bug");
        }
        return inner_->send(request, stop);
    }

private:
    std::shared_ptr<wizz::http::Transport> inner_;
};

TEST(ControllerFlows, An_unexpected_exception_ends_the_panel_red_and_reaches_the_crash_handler)
{
    WindowRigOptions options;
    options.wrap_transport = [](std::shared_ptr<wizz::http::Transport> inner) {
        return std::make_shared<ExplodingTransport>(std::move(inner));
    };
    WindowRig rig{std::move(options)};
    rig.start();
    rig.controller().spawn(rig.controller().search_outbound());
    ASSERT_TRUE(gui::wait_until([&] { return !rig.flow_errors.empty(); }, 10s));
    EXPECT_EQ(rig.busy().status_text(), strings::kUnexpectedErrorText);
    EXPECT_EQ(rig.busy().status_colour(), BusyPanel::kFailureColour);
    EXPECT_FALSE(rig.busy().is_busy()) << "UI-3: the spinner never keeps spinning";
    EXPECT_FALSE(rig.controller().is_working());
    EXPECT_THROW(std::rethrow_exception(rig.flow_errors.front()), std::logic_error);
    for (QWidget* control : rig.input().working_controls()) {
        EXPECT_TRUE(control->isEnabled());
    }
}

TEST(ControllerFlows, Search_entry_points_ignore_calls_while_working)
{
    WindowRigOptions options;
    options.keep_delays = true;
    WindowRig rig{std::move(options)};
    rig.start();
    rig.controller().spawn(rig.controller().search_outbound());
    ASSERT_TRUE(rig.controller().is_working());
    const std::int64_t requests = rig.wizz_requests();
    rig.run(rig.controller().search_outbound());  // returns at once
    rig.run(rig.controller().search_return());
    rig.run(rig.controller().plan_return_from(nullptr));
    EXPECT_TRUE(rig.controller().is_working());
    EXPECT_EQ(rig.wizz_requests(), requests);
    ASSERT_TRUE(gui::wait_until([&] { return !rig.controller().is_working(); }, 15s));
    EXPECT_EQ(rig.busy().status_text(), "Outbound Done: 12 fares from 3 of 3 requests in 1 s.");
}

TEST(ControllerFlows, Nights_and_filters_recompute_the_trips_live)
{
    WindowRig rig;
    rig.start();
    section11_inputs(rig);
    rig.run(rig.controller().search_outbound());
    rig.run(rig.controller().search_return());
    ASSERT_EQ(rig.trips().count(), 22u);

    rig.input().set_max_nights(5);
    rig.input().set_min_nights(2);
    EXPECT_EQ(rig.trips().count(), 19u) << "03 \u00A711 Combine(min 2, max 5)";
    EXPECT_EQ(rig.tab_text(MainWindow::Page::round_trips), "Round trips (19)");

    rig.trips().same_home_box()->setChecked(true);
    EXPECT_LT(rig.trips().count(), 19u);
    EXPECT_TRUE(std::ranges::all_of(rig.trips().trips(), [](const wizz::RoundTrip& trip) {
        return trip.same_home_airport;
    }));
    rig.input().set_min_nights(9);  // max follows (01 §6.2): 9–9 nights
    EXPECT_EQ(rig.input().max_nights(), 9);
    EXPECT_EQ(rig.trips().count(), 0u);
    EXPECT_EQ(rig.trips().summary_text(),
              "No combination with a stay of 9\u20139 nights. Change the nights or the dates.");
    EXPECT_EQ(rig.tab_text(MainWindow::Page::round_trips), "Round trips");
}

TEST(ControllerFlows, The_progress_timer_keeps_ticking_while_a_search_streams)
{
    WindowRigOptions options;
    options.keep_delays = true;
    WindowRig rig{std::move(options)};
    rig.start();
    section11_inputs(rig);
    auto* timer = rig.busy().findChild<QTimer*>(QStringLiteral("animationTimer"));
    ASSERT_NE(timer, nullptr);
    QElapsedTimer clock;
    qint64 last = -1;
    qint64 longest = 0;
    int ticks = 0;
    QObject::connect(timer, &QTimer::timeout, [&] {
        const qint64 now = clock.elapsed();
        if (last >= 0) {
            longest = std::max(longest, now - last);
        }
        last = now;
        ++ticks;
    });
    clock.start();
    rig.run(rig.controller().search_outbound(), 15s);
    EXPECT_GT(ticks, 20);
#ifdef NDEBUG
    EXPECT_LT(longest, 100) << "05 \u00A75: the 40 ms timer's longest gap";
#else
    EXPECT_LT(longest, 250) << "Debug build";
#endif
    RecordProperty("longest_timer_gap_ms", static_cast<int>(longest));
}

} // namespace
