// The --autotest script of 01 §17.2 in-process: input order, screenshot names
// and their fixed order (5 before 4), progress shots, summary.json, the
// exception record, closing unless --keep-open, and settings never saved.

#include "app_test_support.hpp"

#include "autotest_runner.hpp"

#include "wizz/core/http.hpp"
#include "wizz/services/app_paths.hpp"
#include "wizz/ui/strings.hpp"

#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::Date;
using wizz::app::AutoTestRunner;
using wizz::test::app::WindowRig;
using wizz::test::app::WindowRigOptions;
namespace strings = wizz::ui::strings;

/// Records the screenshot names instead of capturing the screen.
struct ShotLog {
    std::vector<std::string> names;
    std::optional<std::string> failure;

    [[nodiscard]] AutoTestRunner::Screenshotter make()
    {
        return [this](const QWidget&, const std::filesystem::path& path) -> std::expected<void, std::string> {
            names.push_back(path.filename().string());
            if (failure) {
                return std::unexpected{*failure};
            }
            return {};
        };
    }

    [[nodiscard]] std::vector<std::string> without_progress() const
    {
        std::vector<std::string> result;
        std::ranges::copy_if(names, std::back_inserter(result),
                             [](const std::string& name) { return !name.starts_with("progress-"); });
        return result;
    }
};

/// --autotest <tmp> --from Budapest --from-radius 200 --to London --to-radius 30
/// --out 2026-11-10:2026-11-20 --ret 2026-11-12:2026-11-20 --then-currency HUF
[[nodiscard]] wizz::ui::AutoTestOptions section11_options(const QTemporaryDir& out)
{
    wizz::ui::AutoTestOptions options;
    options.output_dir = std::filesystem::path{out.path().toStdWString()};
    options.from_radius_km = 200;
    options.to_radius_km = 30;
    options.outbound = wizz::ui::AutoTestDates{.start = Date::from_ymd(2026, 11, 10), .end = Date::from_ymd(2026, 11, 20)};
    options.inbound = wizz::ui::AutoTestDates{.start = Date::from_ymd(2026, 11, 12), .end = Date::from_ymd(2026, 11, 20)};
    options.then_currency = "HUF";
    return options;
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] const wizz::ui::LegJson& leg(const std::optional<wizz::ui::LegDescription>& description)
{
    if (!description || !std::holds_alternative<wizz::ui::LegJson>(*description)) {
        throw std::runtime_error("no leg result");
    }
    return std::get<wizz::ui::LegJson>(*description);
}

TEST(AutoTestRunner, Runs_the_script_takes_the_screenshots_in_order_and_writes_summary_json)
{
    QTemporaryDir out;
    WindowRigOptions options;
    options.autotest = section11_options(out);
    WindowRig rig{std::move(options)};
    ShotLog shots;
    rig.window().autotest_runner()->set_screenshotter(shots.make());
    rig.start();

    EXPECT_EQ(shots.without_progress(), (std::vector<std::string>{"1-outbound.png", "2-return.png", "3-roundtrips.png",
                                                                  "5-currency-switched.png", "4-log.png"}));
    const std::optional<wizz::ui::AutoTestSummary>& summary = rig.window().autotest_runner()->summary();
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(summary->started_at, "2026-11-02T10:00:00");
    EXPECT_EQ(summary->api_url, "https://be.wizzair.com/29.23.1/Api");
    EXPECT_EQ(summary->airports, 11u);
    EXPECT_EQ(leg(summary->outbound).flights, 12u);
    EXPECT_EQ(leg(summary->outbound).requests_planned, 3u);
    EXPECT_EQ(leg(summary->outbound).view, "AllFares");
    EXPECT_GT(leg(summary->outbound).progress_updates, 0);
    EXPECT_EQ(leg(summary->outbound).origin_airports, (std::vector<std::string>{"BUD 0km", "BTS 173km", "DEB 184km"}));
    EXPECT_EQ(leg(summary->inbound).flights, 4u);
    ASSERT_TRUE(summary->round_trips.has_value());
    EXPECT_EQ(summary->round_trips->count, 22u);
    ASSERT_TRUE(summary->after_currency_switch.has_value());
    EXPECT_EQ(summary->after_currency_switch->currency, "HUF");
    const auto& switched = std::get<wizz::ui::LegJson>(summary->after_currency_switch->outbound);
    EXPECT_EQ(switched.progress_updates, 0);
    EXPECT_EQ(switched.current_row, "2026-11-12 BTS->LTN 5491");
    EXPECT_EQ(summary->after_currency_switch->round_trips.count, 22u);
    EXPECT_FALSE(summary->exception.has_value());
    EXPECT_EQ(summary->final_status, "Return Done: 4 fares from 4 of 4 requests in 1 s.");
    EXPECT_EQ(summary->requests_to_wizz, 9) << "2 + 3 + 4: the currency switch added none";
    ASSERT_FALSE(summary->log.empty());
    EXPECT_EQ(summary->log.front(), std::string{"[10:00:00] "} + std::string{strings::kStartedLog});

    // The file itself, and the window closed without saving settings.
    const std::string json = read_text(rig.window().autotest_runner()->summary_path());
    EXPECT_TRUE(json.contains("\"requestsToWizz\": 9")) << json.substr(0, 400);
    EXPECT_LT(json.find("\"afterCurrencySwitch\""), json.find("\"progressScreenshots\""));
    EXPECT_FALSE(rig.window().isVisible()) << "closed at the end without --keep-open";
    EXPECT_FALSE(std::filesystem::exists(wizz::app_paths::settings_file(rig.data_root())));
    EXPECT_FALSE(rig.window().autotest_runner()->diagnostics().has_value()) << "only with --diagnostics";
    EXPECT_FALSE(std::filesystem::exists(rig.window().autotest_runner()->diagnostics_path()));

    // The inputs the script applied (01 §17.2).
    EXPECT_EQ(rig.input().from_radius_km(), 200.0);
    EXPECT_EQ(rig.input().out_from(), Date::from_ymd(2026, 11, 10));
    EXPECT_EQ(rig.input().out_to(), Date::from_ymd(2026, 11, 20));
    EXPECT_EQ(rig.input().ret_from(), Date::from_ymd(2026, 11, 12));
    EXPECT_EQ(rig.input().ret_to(), Date::from_ymd(2026, 11, 20));
    EXPECT_EQ(rig.input().currency(), "HUF");
}

TEST(AutoTestRunner, Progress_screenshots_every_2200_ms_while_working)
{
    QTemporaryDir out;
    WindowRigOptions options;
    options.keep_delays = true;  // 700 ms per timetable answer: about 5 s of searching
    options.autotest = section11_options(out);
    options.autotest->then_currency.reset();
    WindowRig rig{std::move(options)};
    ShotLog shots;
    rig.window().autotest_runner()->set_screenshotter(shots.make());
    rig.start();

    std::vector<std::string> progress;
    std::ranges::copy_if(shots.names, std::back_inserter(progress),
                         [](const std::string& name) { return name.starts_with("progress-"); });
    ASSERT_GE(progress.size(), 1u);
    ASSERT_LE(progress.size(), static_cast<std::size_t>(AutoTestRunner::kMaxProgressShots));
    for (std::size_t i = 0; i < progress.size(); ++i) {
        EXPECT_EQ(progress[i], "progress-" + std::to_string(i + 1) + ".png");
    }
    const auto& summary = rig.window().autotest_runner()->summary();
    ASSERT_TRUE(summary.has_value());
    ASSERT_EQ(summary->progress_screenshots.size(), progress.size());
    // Each shot shows a status of a running search: mostly "Fetching fares" (the server's
    // 700 ms answers dominate), but on a slow machine a 2200 ms tick may also land in a
    // search's first or last phase. Never the line a finished search leaves while idle.
    for (const std::string& status : summary->progress_screenshots) {
        EXPECT_FALSE(status.empty());
        EXPECT_FALSE(status.starts_with("Ready") || status.starts_with("Outbound Done") ||
                     status.starts_with("Return Done"))
            << status;
    }
    EXPECT_FALSE(summary->after_currency_switch.has_value());
    EXPECT_EQ(shots.without_progress(),
              (std::vector<std::string>{"1-outbound.png", "2-return.png", "3-roundtrips.png", "4-log.png"}));
}

TEST(AutoTestRunner, A_failed_screenshot_is_logged_and_the_script_goes_on)
{
    QTemporaryDir out;
    WindowRigOptions options;
    options.scenario = "same_area";
    options.autotest = wizz::ui::AutoTestOptions{};
    options.autotest->output_dir = std::filesystem::path{out.path().toStdWString()};
    options.autotest->to = "BUD";
    options.autotest->keep_open = true;
    WindowRig rig{std::move(options)};
    ShotLog shots;
    shots.failure = "no screen";
    rig.window().autotest_runner()->set_screenshotter(shots.make());
    rig.start();

    EXPECT_EQ(shots.names, (std::vector<std::string>{"1-outbound.png", "4-log.png"}));
    EXPECT_TRUE(rig.log_contains("screenshot 1-outbound.png failed: no screen"));
    EXPECT_TRUE(rig.window().isVisible()) << "--keep-open";
    const auto& summary = rig.window().autotest_runner()->summary();
    ASSERT_TRUE(summary.has_value());
    EXPECT_TRUE(std::holds_alternative<wizz::ui::NoResult>(*summary->outbound)) << "0 requests: no result stored";
    EXPECT_FALSE(summary->inbound.has_value());
    EXPECT_EQ(summary->requests_to_wizz, 2);
    EXPECT_TRUE(std::filesystem::exists(rig.window().autotest_runner()->summary_path()));
    // The failure line is written before the summary, so the summary log has the first one.
    EXPECT_TRUE(std::ranges::any_of(summary->log, [](const std::string& line) {
        return line.ends_with("screenshot 1-outbound.png failed: no screen");
    }));
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

TEST(AutoTestRunner, An_exception_is_recorded_in_the_summary)
{
    QTemporaryDir out;
    WindowRigOptions options;
    options.autotest = section11_options(out);
    options.wrap_transport = [](std::shared_ptr<wizz::http::Transport> inner) {
        return std::make_shared<ExplodingTransport>(std::move(inner));
    };
    WindowRig rig{std::move(options)};
    ShotLog shots;
    rig.window().autotest_runner()->set_screenshotter(shots.make());
    rig.start();

    const auto& summary = rig.window().autotest_runner()->summary();
    ASSERT_TRUE(summary.has_value());
    ASSERT_TRUE(summary->exception.has_value());
    EXPECT_TRUE(summary->exception->contains("std::logic_error")) << *summary->exception;
    EXPECT_TRUE(summary->exception->contains("unexpected bug")) << *summary->exception;
    EXPECT_FALSE(summary->outbound.has_value()) << "the script stopped inside the outbound search";
    EXPECT_EQ(summary->final_status, strings::kUnexpectedErrorText);
    EXPECT_TRUE(shots.without_progress().empty());
    EXPECT_TRUE(rig.flow_errors.empty()) << "the script's catch handled it, not the crash handler";
}

// --- D-30 acceptance companions (05 §5) ---------------------------------------------------

TEST(AutoTestRunner, D_30_Warm_cache_makes_the_scripts_search_a_cached_replay)
{
    QTemporaryDir out;
    WindowRigOptions options;
    options.autotest = section11_options(out);
    options.autotest->inbound.reset();
    options.autotest->then_currency.reset();
    options.autotest->warm_cache = true;
    options.autotest->diagnostics = true;
    WindowRig rig{std::move(options)};
    ShotLog shots;
    rig.window().autotest_runner()->set_screenshotter(shots.make());
    rig.start();

    const auto& summary = rig.window().autotest_runner()->summary();
    ASSERT_TRUE(summary.has_value());
    EXPECT_EQ(leg(summary->outbound).flights, 12u);
    EXPECT_EQ(leg(summary->outbound).requests_completed, 3);
    EXPECT_EQ(summary->requests_to_wizz, 2 + 3) << "homepage, map and the warm-up; the replay made none";
    EXPECT_EQ(std::ranges::count(summary->log, std::string{"[10:00:00] Outbound: 12 fares, 3/3 requests, 1 route(s) "
                                                           "not served, 1 s, FX Frankfurter 2026-11-02"}),
              2);
    // The replay reused every fare of the warm-up and says so once, in its own result (D-70).
    EXPECT_EQ(std::ranges::count_if(summary->log, [](const std::string& line) { return line.contains("Reused fares"); }),
              1);
    EXPECT_EQ(std::ranges::count(summary->log, std::string{"[10:00:00]   Reused fares fetched in the last minute: 3 of 3 "
                                                           "requests answered from them."}),
              1);

    const auto& diagnostics = rig.window().autotest_runner()->diagnostics();
    ASSERT_TRUE(diagnostics.has_value());
    ASSERT_EQ(diagnostics->searches.size(), 2u);
    EXPECT_EQ(diagnostics->searches[0].name, "warm-up outbound");
    EXPECT_EQ(diagnostics->searches[0].wizz_requests, 3);
    EXPECT_EQ(diagnostics->searches[1].name, "outbound");
    EXPECT_EQ(diagnostics->searches[1].wizz_requests, 0);
    EXPECT_EQ(diagnostics->searches[1].final_status, "Outbound Done: 12 fares from 3 of 3 requests in 1 s.");
    ASSERT_EQ(diagnostics->grids.size(), 2u);
    EXPECT_EQ(diagnostics->grids[0].rows, 12u);
    EXPECT_EQ(diagnostics->grids[0].result_flights, 12u);
    EXPECT_EQ(diagnostics->grids[0].rows_not_in_result, 0u) << "no row of the warm-up search is left";
    EXPECT_EQ(diagnostics->grids[0].duplicate_rows, 0u);
    EXPECT_FALSE(diagnostics->cancel.has_value());
    EXPECT_GE(diagnostics->timer.runs, 4) << "two searches: Finding places and Fetching fares each";
    ASSERT_TRUE(diagnostics->events.has_value()) << "the GUI tests run on wizz::app::Application";
    EXPECT_GT(diagnostics->events->dispatches, 0);
    EXPECT_EQ(diagnostics->layout.checks, 2) << "one per screenshot: 1-outbound, 4-log";
    EXPECT_EQ(diagnostics->environment.locale, "en-US");

    const std::string json = read_text(rig.window().autotest_runner()->diagnostics_path());
    EXPECT_TRUE(json.contains("\"name\": \"warm-up outbound\"")) << json.substr(0, 400);
    EXPECT_TRUE(json.contains("\"uiEvents\": {"));
}

TEST(AutoTestRunner, D_30_Cancel_after_ms_presses_Esc_and_Cancel_with_one_transition)
{
    // The cancel_waiting server answers a timetable request only after a long delay, so
    // the cancel 300 ms into the fetch lands while the first request waits — on any
    // machine. Half that delay bounds the cancelled search: far more than any scheduling
    // delay of a loaded runner, far less than a search that waited the request out.
    std::chrono::milliseconds server_delay{0};
    for (const wizz::ui::ReplayTransport::Rule& rule : wizz::test::app::scenario_rules("cancel_waiting", true)) {
        server_delay = std::max(server_delay, rule.delay);
    }
    ASSERT_GE(server_delay, 10s);
    QTemporaryDir out;
    WindowRigOptions options;
    options.scenario = "cancel_waiting";
    options.keep_delays = true;
    options.autotest = section11_options(out);
    options.autotest->inbound.reset();
    options.autotest->then_currency.reset();
    options.autotest->cancel_after = 300ms;
    options.autotest->diagnostics = true;
    WindowRig rig{std::move(options)};
    ShotLog shots;
    rig.window().autotest_runner()->set_screenshotter(shots.make());
    int cancel_requests = 0;
    QObject::connect(&rig.busy(), &wizz::app::BusyPanel::cancel_requested, [&] { ++cancel_requests; });
    rig.start();

    EXPECT_EQ(cancel_requests, 1) << "Esc, Cancel, Esc and the handler: one transition (D-11)";
    const auto& summary = rig.window().autotest_runner()->summary();
    ASSERT_TRUE(summary.has_value());
    EXPECT_TRUE(leg(summary->outbound).cancelled);
    EXPECT_EQ(leg(summary->outbound).requests_completed, 0);
    EXPECT_EQ(summary->final_status, "Outbound Cancelled: 0 fares from 0 of 3 requests in 1 s.");
    EXPECT_EQ(summary->requests_to_wizz, 2 + 1);

    const auto& diagnostics = rig.window().autotest_runner()->diagnostics();
    ASSERT_TRUE(diagnostics.has_value());
    ASSERT_TRUE(diagnostics->cancel.has_value());
    const wizz::app::CancelDiagnostics& cancel = *diagnostics->cancel;
    EXPECT_EQ(cancel.after, 300ms);
    EXPECT_GE(cancel.pressed_at_ms, 290.0);
    EXPECT_TRUE(cancel.busy_at_press);
    EXPECT_TRUE(cancel.status_before.starts_with("Fetching fares:  BUD")) << cancel.status_before;
    EXPECT_EQ(cancel.presses, (std::vector<std::string>{"Esc", "Cancel button", "Esc", "Cancel handler"}));
    EXPECT_EQ(cancel.cancel_requests, 1);
    EXPECT_EQ(cancel.status_after_press, strings::kCancelling);
    EXPECT_FALSE(cancel.cancel_enabled_after_press);
    EXPECT_LT(diagnostics->searches.at(0).duration_ms,
              static_cast<double>(server_delay.count()) / 2)
        << "the server's wait was interrupted";
}

TEST(AutoTestRunner, D_30_A_cancel_timer_that_fires_after_the_search_presses_nothing)
{
    QTemporaryDir out;
    WindowRigOptions options;
    options.autotest = section11_options(out);
    options.autotest->cancel_after = std::chrono::milliseconds{60'000};  // the search ends long before
    options.autotest->then_currency.reset();
    options.autotest->diagnostics = true;
    WindowRig rig{std::move(options)};
    ShotLog shots;
    rig.window().autotest_runner()->set_screenshotter(shots.make());
    rig.start();
    const auto& diagnostics = rig.window().autotest_runner()->diagnostics();
    ASSERT_TRUE(diagnostics.has_value());
    EXPECT_FALSE(diagnostics->cancel.has_value()) << "stopped with the outbound search, never carried into the return";
    EXPECT_EQ(rig.window().autotest_runner()->summary()->final_status,
              "Return Done: 4 fares from 4 of 4 requests in 1 s.");
}

} // namespace
