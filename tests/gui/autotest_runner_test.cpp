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
    rig.start(20s);

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
    rig.start(30s);

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
    for (const std::string& status : summary->progress_screenshots) {
        EXPECT_TRUE(status.starts_with("Fetching fares")) << status;
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

} // namespace
