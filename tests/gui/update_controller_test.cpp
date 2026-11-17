// The self-updater in the main window (D-45..D-49): it checks on a worker after the
// window is shown when the core says a check is due, re-evaluates every hour, never
// runs in self-test runs, stays silent apart from Log lines, downloads the installer
// of an installed copy before offering "Restart and update", links a portable copy
// to the release page, and starts the silent installer when the window closes —
// through a recording launcher, never a real process.

#include "update_test_support.hpp"

#include "autotest_runner.hpp"
#include "input_panel.hpp"
#include "log_view.hpp"
#include "place_combo.hpp"
#include "search_button.hpp"

#include "support/fake_transport.hpp"
#include "wizz/services/app_paths.hpp"
#include "wizz/ui/settings.hpp"

#include <QLabel>
#include <QPushButton>
#include <QTimer>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::test::app::FakeUpdateBackend;
using wizz::test::app::kRunningVersion;
using wizz::test::app::release_info;
using wizz::test::app::update_error;
using wizz::test::app::UpdateRig;
using wizz::test::app::UpdateRigOptions;
using wizz::test::app::WindowRig;
using wizz::test::app::WindowRigOptions;
namespace gui = wizz::test::gui;
namespace update = wizz::update;
using update::InstallKind;
using update::UpdateErrorKind;
using update::Version;

const Version kNewVersion{1, 2, 0};
constexpr std::string_view kReadyText =
    "WizzairHelper 1.2.0 is ready to install. It will be installed when you close WizzairHelper.";
constexpr std::string_view kReleasePage = "https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v1.2.0";

[[nodiscard]] QString q(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// The Log lines that mention updates.
[[nodiscard]] std::vector<std::string> update_lines(const WindowRig& rig)
{
    std::vector<std::string> lines;
    std::ranges::copy_if(rig.log_lines(), std::back_inserter(lines),
                         [](const std::string& line) { return line.starts_with("Update"); });
    return lines;
}

/// An installed copy whose check finds 1.2.0, downloaded and verified: the bar is up.
void reach_ready(UpdateRig& rig)
{
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.start();
    ASSERT_TRUE(gui::wait_until([&] { return rig.bar().isVisible(); })) << "the ready bar did not appear";
}

[[nodiscard]] std::wstring expected_command_line(const UpdateRig& rig, bool relaunch)
{
    std::wstring line = L"\"" + rig.backend->installer_path(kNewVersion).wstring() +
                        L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS";
    if (relaunch) {
        line += L" /RELAUNCH";
    }
    return line;
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

TEST(UpdateController, Checks_on_a_worker_after_the_window_is_shown_when_a_check_is_due)
{
    UpdateRig rig;
    ASSERT_TRUE(rig.enable());
    ASSERT_NE(rig.controller(), nullptr);
    EXPECT_FALSE(rig.controller()->is_started());
    gui::settle();
    EXPECT_EQ(rig.backend->evaluations(), 0) << "nothing happens before the window is shown";

    rig.window_rig.start();
    rig.wait_for_evaluations(1);
    EXPECT_TRUE(rig.controller()->is_started());
    EXPECT_EQ(rig.backend->checks(), 1) << "a fresh profile is due";
    EXPECT_EQ(rig.backend->cleanups(), 1) << "old downloads are removed once";
    const std::vector<std::thread::id> threads = rig.backend->worker_threads();
    ASSERT_EQ(threads.size(), 1u);
    EXPECT_NE(threads.front(), std::this_thread::get_id()) << "the check never runs on the UI thread";

    // No release published: silent, nothing shown.
    EXPECT_FALSE(rig.bar().isVisible());
    EXPECT_TRUE(update_lines(rig.window_rig).empty());
    EXPECT_TRUE(rig.window_rig.dialogs.errors.empty());
    EXPECT_TRUE(rig.window_rig.actions.messages.empty());
    EXPECT_FALSE(rig.backend->schedule().is_check_due()) << "a 404 counts as a successful check (D-40)";

    // The hourly re-evaluation.
    const QTimer* timer = rig.controller()->recheck_timer();
    ASSERT_NE(timer, nullptr);
    EXPECT_TRUE(timer->isActive());
    EXPECT_EQ(std::chrono::milliseconds{timer->interval()}, std::chrono::milliseconds{std::chrono::hours{1}});
}

TEST(UpdateController, Does_not_check_when_no_check_is_due)
{
    UpdateRig rig;
    rig.backend->schedule().record_success(std::nullopt);  // checked at 10:00 today
    rig.clock->advance(std::chrono::hours{13} + std::chrono::minutes{59});  // 23:59, the same day
    rig.start();
    EXPECT_EQ(rig.backend->evaluations(), 1);
    EXPECT_EQ(rig.backend->checks(), 0) << "no request when the core says no check is due";

    rig.controller()->evaluate();
    rig.wait_for_evaluations(2);
    EXPECT_EQ(rig.backend->checks(), 0);
    EXPECT_FALSE(rig.bar().isVisible());
}

TEST(UpdateController, Re_evaluates_every_hour_so_a_window_left_open_checks_once_a_day)
{
    UpdateRigOptions options;
    options.interval = 20ms;  // the hour, compressed
    UpdateRig rig{std::move(options)};
    rig.start();
    EXPECT_EQ(rig.backend->checks(), 1);

    // Hours pass: the timer keeps asking, the core keeps saying "not due".
    rig.wait_for_evaluations(4);
    EXPECT_EQ(rig.backend->checks(), 1);

    rig.clock->advance(std::chrono::hours{24});  // a day later
    // The first evaluation that starts after the jump must check: it reads the new time.
    const int started = rig.backend->evaluations();
    rig.wait_for_evaluations(started + 1);
    EXPECT_EQ(rig.backend->checks(), 2) << "due again a day later";
    rig.wait_for_evaluations(rig.backend->evaluations() + 3);
    EXPECT_EQ(rig.backend->checks(), 2) << "once per day";
}

TEST(UpdateController, A_failed_check_is_retried_by_the_core_schedule_an_hour_later)
{
    UpdateRigOptions options;
    options.interval = 20ms;
    UpdateRig rig{std::move(options)};
    rig.backend->script_checks({std::unexpected(update_error(UpdateErrorKind::http_status,
                                                             "Update check failed: GitHub answered HTTP 503.")),
                                std::unexpected(update_error(UpdateErrorKind::no_release, "No release."))});
    rig.start();
    EXPECT_EQ(rig.backend->checks(), 1);
    rig.wait_for_evaluations(4);
    EXPECT_EQ(rig.backend->checks(), 1) << "not before an hour has passed";
    rig.clock->advance(std::chrono::minutes{61});
    // The first evaluation that starts after the jump must retry: it reads the new time.
    const int started = rig.backend->evaluations();
    rig.wait_for_evaluations(started + 1);
    EXPECT_EQ(rig.backend->checks(), 2) << "retried once the hour has passed";
}

TEST(UpdateController, Is_never_active_in_self_test_runs)
{
    QTemporaryDir out;
    WindowRigOptions window;
    window.autotest = wizz::ui::AutoTestOptions{};
    window.autotest->output_dir = std::filesystem::path{out.path().toStdWString()};
    window.autotest->replay_dir = wizz::test::app::replay_root() / "named_destination";
    window.autotest->keep_open = true;
    UpdateRigOptions options;
    options.window = std::move(window);
    UpdateRig rig{std::move(options)};
    EXPECT_FALSE(rig.enable()) << "--autotest (and with it --replay) never checks";
    EXPECT_EQ(rig.controller(), nullptr);
    rig.window_rig.window().autotest_runner()->set_screenshotter(
        [](const QWidget&, const std::filesystem::path&) -> std::expected<void, std::string> { return {}; });
    rig.window_rig.start();
    gui::settle();
    EXPECT_EQ(rig.backend->evaluations(), 0);
    EXPECT_FALSE(rig.bar().isVisible());
    rig.window().close();
    EXPECT_TRUE(rig.launches.commands.empty());
}

TEST(UpdateController, Is_enabled_once)
{
    UpdateRig rig;
    EXPECT_TRUE(rig.enable());
    EXPECT_FALSE(rig.enable());
}

// ---------------------------------------------------------------------------
// Offers
// ---------------------------------------------------------------------------

TEST(UpdateController, An_installed_copy_downloads_and_verifies_then_offers_restart)
{
    UpdateRig rig;
    rig.start();  // up to date at start-up
    ASSERT_FALSE(rig.bar().isVisible());
    QWidget* const focus = rig.window_rig.input().to_box();
    focus->setFocus();
    ASSERT_EQ(rig.window().focusWidget(), focus);

    // A day later the hourly re-evaluation finds 1.2.0.
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.clock->advance(std::chrono::hours{24});
    rig.controller()->evaluate();
    ASSERT_TRUE(gui::wait_until([&] { return rig.bar().isVisible(); }));
    gui::settle();

    EXPECT_EQ(rig.backend->downloaded(), std::vector<Version>{kNewVersion});
    for (const std::thread::id thread : rig.backend->worker_threads()) {
        EXPECT_NE(thread, std::this_thread::get_id());
    }
    EXPECT_EQ(update_lines(rig.window_rig),
              (std::vector<std::string>{"Update available: WizzairHelper 1.2.0 (running 1.0.0).",
                                        "Update 1.2.0 downloaded and verified."}));
    EXPECT_EQ(rig.bar().message_label()->text(), q(kReadyText));
    EXPECT_EQ(rig.bar().action_button()->text(), "Restart and update");
    EXPECT_TRUE(rig.bar().action_button()->isVisible());
    EXPECT_TRUE(rig.bar().action_button()->isEnabled());
    EXPECT_EQ(rig.bar().later_button()->text(), "Later");
    EXPECT_TRUE(rig.bar().later_button()->isVisible());
    EXPECT_EQ(rig.window().focusWidget(), focus) << "the bar never takes the focus";
    EXPECT_TRUE(rig.window_rig.dialogs.errors.empty());
    EXPECT_TRUE(rig.window_rig.actions.messages.empty());
    EXPECT_TRUE(rig.launches.commands.empty()) << "nothing starts before the window closes";

    // The bar sits between the input panel and the result tabs (central-widget coordinates:
    // the tabs are inside the results card, D-53).
    const QWidget* central = rig.window().centralWidget();
    const auto in_central = [central](const QWidget* widget) {
        return QRect(widget->mapTo(central, QPoint(0, 0)), widget->size());
    };
    const QRect input = in_central(rig.window().input_panel());
    const QRect bar = in_central(&rig.bar());
    const QRect tabs = in_central(rig.window().tabs());
    EXPECT_LE(input.bottom(), bar.top());
    EXPECT_LE(bar.bottom(), tabs.top());
    EXPECT_EQ(bar.width(), central->width());
}

TEST(UpdateController, A_portable_copy_links_to_the_release_page)
{
    UpdateRigOptions options;
    options.kind = InstallKind::portable;
    UpdateRig rig{std::move(options)};
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.start();
    ASSERT_TRUE(rig.bar().isVisible());

    EXPECT_TRUE(rig.backend->downloaded().empty()) << "a portable folder is never replaced";
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(rig.bar().action_button()->text(), "Download");
    EXPECT_TRUE(rig.bar().action_button()->isEnabled());
    EXPECT_EQ(update_lines(rig.window_rig),
              std::vector<std::string>{"Update available: WizzairHelper 1.2.0 (running 1.0.0)."});

    rig.bar().action_button()->click();
    EXPECT_EQ(rig.launches.opened_pages, std::vector<std::string>{std::string{kReleasePage}});
    EXPECT_TRUE(rig.bar().isVisible());
    EXPECT_TRUE(rig.window().isVisible()) << "Download only opens the page";

    rig.window().close();
    EXPECT_TRUE(rig.launches.commands.empty());
    EXPECT_EQ(rig.launches.mutex_releases, 0);
}

TEST(UpdateController, A_failed_check_is_one_Log_line_and_nothing_else)
{
    UpdateRig rig;
    rig.backend->script_checks(
        {std::unexpected(update_error(UpdateErrorKind::http_status, "Update check failed: GitHub answered HTTP 500."))});
    rig.start();
    EXPECT_EQ(update_lines(rig.window_rig), std::vector<std::string>{"Update check failed: GitHub answered HTTP 500."});
    EXPECT_FALSE(rig.bar().isVisible());
    EXPECT_TRUE(rig.window_rig.dialogs.errors.empty());
    EXPECT_TRUE(rig.window_rig.actions.messages.empty());

    // The schedule's retry an hour later fails the same way: one more line, focus untouched.
    QWidget* const focus = rig.window_rig.input().to_box();
    focus->setFocus();
    rig.clock->advance(std::chrono::minutes{61});
    rig.controller()->evaluate();
    rig.wait_for_evaluations(2);
    EXPECT_EQ(rig.backend->checks(), 2);
    EXPECT_EQ(update_lines(rig.window_rig), (std::vector<std::string>{"Update check failed: GitHub answered HTTP 500.",
                                                                      "Update check failed: GitHub answered HTTP 500."}));
    EXPECT_EQ(rig.window().focusWidget(), focus) << "no focus change";
    EXPECT_TRUE(rig.window_rig.dialogs.errors.empty());
}

TEST(UpdateController, A_failed_download_is_logged_and_tried_again_at_the_next_evaluation)
{
    UpdateRig rig;
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.backend->script_downloads(
        {std::unexpected(update_error(UpdateErrorKind::http_status,
                                      "Could not download WizzairHelper-1.2.0-win64-setup.exe after 5 tries: HTTP 503.")),
         wizz::app::UpdateDownloadResult{update::DownloadedFile{
             .path = rig.backend->installer_path(kNewVersion), .sha256 = std::string(64, 'b'), .reused = false, .tries = {}}}});
    rig.start();
    EXPECT_FALSE(rig.bar().isVisible());
    EXPECT_EQ(update_lines(rig.window_rig),
              (std::vector<std::string>{
                  "Update available: WizzairHelper 1.2.0 (running 1.0.0).",
                  "Update download failed: Could not download WizzairHelper-1.2.0-win64-setup.exe after 5 tries: HTTP 503."}));

    rig.controller()->evaluate();  // the next hourly tick
    ASSERT_TRUE(gui::wait_until([&] { return rig.bar().isVisible(); }));
    EXPECT_EQ(rig.backend->downloaded(), (std::vector<Version>{kNewVersion, kNewVersion}));
    EXPECT_EQ(rig.backend->checks(), 1) << "the download is retried without another check";
    EXPECT_EQ(update_lines(rig.window_rig).back(), "Update 1.2.0 downloaded and verified.");
}

// ---------------------------------------------------------------------------
// The bar's buttons
// ---------------------------------------------------------------------------

TEST(UpdateController, Restart_and_update_is_disabled_while_a_search_works)
{
    UpdateRigOptions options;
    options.window.keep_delays = true;  // the named_destination server takes its time
    UpdateRig rig{std::move(options)};
    reach_ready(rig);
    gui::settle();
    ASSERT_FALSE(rig.window_rig.controller().is_working());
    EXPECT_TRUE(rig.bar().action_button()->isEnabled());

    rig.window_rig.controller().request_search_outbound();
    ASSERT_TRUE(gui::wait_until([&] { return rig.window_rig.controller().is_working(); }));
    EXPECT_FALSE(rig.bar().action_button()->isEnabled());
    EXPECT_TRUE(rig.bar().later_button()->isEnabled());
    rig.controller()->trigger_action();  // e.g. a click that raced the disabling
    gui::settle();
    EXPECT_TRUE(rig.window().isVisible()) << "nothing restarts while a search works";
    EXPECT_TRUE(rig.launches.commands.empty());

    rig.window_rig.controller().cancel_work();
    ASSERT_TRUE(gui::wait_until([&] { return !rig.window_rig.controller().is_working(); }));
    EXPECT_TRUE(rig.bar().action_button()->isEnabled());
}

TEST(UpdateController, Later_hides_the_bar_for_the_session_and_the_update_still_installs_on_exit)
{
    UpdateRig rig;
    reach_ready(rig);
    rig.bar().later_button()->click();
    EXPECT_FALSE(rig.bar().isVisible());
    EXPECT_TRUE(rig.window().isVisible());

    rig.controller()->evaluate();
    rig.wait_for_evaluations(2);
    EXPECT_FALSE(rig.bar().isVisible()) << "hidden for the rest of the session";

    rig.window().close();
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_EQ(rig.launches.commands.front().command_line(), expected_command_line(rig, false));
}

// ---------------------------------------------------------------------------
// Exit
// ---------------------------------------------------------------------------

TEST(UpdateController, Closing_with_a_verified_installer_starts_the_silent_install)
{
    UpdateRig rig;
    reach_ready(rig);
    rig.window().close();

    ASSERT_EQ(rig.launches.commands.size(), 1u);
    const update::InstallerCommand& command = rig.launches.commands.front();
    EXPECT_EQ(command.program, rig.backend->installer_path(kNewVersion));
    EXPECT_EQ(command.arguments,
              (std::vector<std::string>{"/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS"}));
    EXPECT_EQ(command.command_line(), expected_command_line(rig, false));
    EXPECT_EQ(rig.launches.mutex_releases, 1);
    EXPECT_EQ(rig.launches.released_before_launch, std::vector<bool>{true})
        << "the installer must not find the app mutex (D-47)";
    EXPECT_EQ(update_lines(rig.window_rig).back(), "Update 1.2.0: installer started.");
    const std::vector<std::string> log = rig.backend->log_lines();
    ASSERT_FALSE(log.empty());
    EXPECT_EQ(log.back(), std::format("Started {} /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS",
                                      "\"" + rig.backend->installer_path(kNewVersion).string() + "\""));
    EXPECT_TRUE(std::filesystem::exists(wizz::app_paths::settings_file(rig.window_rig.data_root())))
        << "the settings were saved as usual";
    EXPECT_TRUE(rig.controller()->is_closing());
    EXPECT_FALSE(rig.controller()->recheck_timer()->isActive());
}

TEST(UpdateController, Restart_and_update_saves_the_settings_starts_the_installer_with_RELAUNCH_and_closes)
{
    UpdateRig rig;
    reach_ready(rig);
    rig.window_rig.input().set_from_text("Graz");
    rig.bar().action_button()->click();

    EXPECT_FALSE(rig.window().isVisible()) << "the window closed";
    ASSERT_EQ(rig.launches.commands.size(), 1u) << "started once";
    EXPECT_EQ(rig.launches.commands.front().command_line(), expected_command_line(rig, true));
    EXPECT_EQ(rig.launches.released_before_launch, std::vector<bool>{true});
    const std::filesystem::path settings = wizz::app_paths::settings_file(rig.window_rig.data_root());
    ASSERT_TRUE(std::filesystem::exists(settings));
    EXPECT_EQ(wizz::ui::AppSettings::load(settings).from, "Graz");
}

TEST(UpdateController, A_failed_installer_start_is_logged_and_the_window_still_closes)
{
    UpdateRig rig;
    rig.launches.launch_error = "Access is denied.";
    reach_ready(rig);
    rig.window().close();
    EXPECT_FALSE(rig.window().isVisible());
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_EQ(update_lines(rig.window_rig).back(),
              "Update 1.2.0: the installer could not be started: Access is denied.");
    const std::vector<std::string> log = rig.backend->log_lines();
    ASSERT_FALSE(log.empty());
    EXPECT_TRUE(log.back().starts_with("Could not start \"")) << log.back();
    EXPECT_TRUE(log.back().ends_with(": Access is denied.")) << log.back();
}

TEST(UpdateController, Nothing_starts_without_a_verified_installer)
{
    {
        UpdateRig up_to_date;
        up_to_date.start();
        up_to_date.window().close();
        EXPECT_TRUE(up_to_date.launches.commands.empty());
        EXPECT_EQ(up_to_date.launches.mutex_releases, 0);
    }
    {
        UpdateRig failed;
        failed.backend->script_checks({release_info(kNewVersion)});
        failed.backend->script_downloads({std::unexpected(update_error(
            UpdateErrorKind::checksum_mismatch, "WizzairHelper-1.2.0-win64-setup.exe does not match SHA256SUMS.txt."))});
        failed.start();
        failed.window().close();
        EXPECT_TRUE(failed.launches.commands.empty());
    }
}

// ---------------------------------------------------------------------------
// The user's options (D-59..D-62)
// ---------------------------------------------------------------------------

[[nodiscard]] UpdateRigOptions with_updates(update::UpdateOptions updates,
                                            InstallKind kind = InstallKind::installed)
{
    UpdateRigOptions options;
    options.kind = kind;
    options.window.settings.updates = updates;
    return options;
}

[[nodiscard]] update::UpdateOptions choice(update::WhenFound when_found, bool automatic = true,
                                           update::CheckFrequency frequency = update::CheckFrequency::daily)
{
    return update::UpdateOptions{.check_automatically = automatic, .check_frequency = frequency, .when_found = when_found};
}

/// Waits until the controller has no work running or queued and no manual check open.
void wait_idle(UpdateRig& rig)
{
    ASSERT_TRUE(gui::wait_until([&] { return !rig.controller()->is_running() && !rig.controller()->model().checking(); }));
    gui::settle();
}

TEST(UpdateController, Takes_the_options_from_settings_json)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::notify_only, true, update::CheckFrequency::monthly))};
    rig.start();
    EXPECT_EQ(rig.controller()->options(), choice(update::WhenFound::notify_only, true, update::CheckFrequency::monthly));
    const std::vector<update::CheckPolicy> policies = rig.backend->policies();
    ASSERT_FALSE(policies.empty());
    EXPECT_TRUE(policies.front().automatic);
    EXPECT_EQ(policies.front().frequency, update::CheckFrequency::monthly);
    EXPECT_EQ(policies.front().recheck_if_newer_than, kRunningVersion) << "a session's first evaluation";
}

TEST(UpdateController, Turned_off_it_sends_nothing_until_Check_now)
{
    UpdateRigOptions options = with_updates(choice(update::WhenFound::install_on_close, false));
    options.interval = 20ms;
    UpdateRig rig{std::move(options)};
    rig.start();
    rig.wait_for_evaluations(3);
    EXPECT_EQ(rig.backend->checks(), 0) << "no request to GitHub at all";
    EXPECT_FALSE(rig.backend->schedule().is_check_due(rig.controller()->model().check_policy()));

    rig.controller()->check_now();
    // The ticks keep evaluating meanwhile; wait for the manual check's own answer.
    ASSERT_TRUE(gui::wait_until([&] { return !rig.controller()->model().checking(); }));
    EXPECT_EQ(rig.backend->manual_checks(), 1);
    EXPECT_EQ(rig.backend->checks(), 1);
    EXPECT_EQ(rig.controller()->status_view().kind, wizz::ui::UpdateStatusKind::up_to_date);
    const int evaluations = rig.backend->evaluations();
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->evaluations() >= evaluations + 2; }));
    EXPECT_EQ(rig.backend->checks(), 1) << "and nothing automatic after it";
}

TEST(UpdateController, Once_a_week_checks_again_in_the_next_ISO_week)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::install_on_close, true, update::CheckFrequency::weekly))};
    rig.start();  // Monday 2 Nov 2026, 10:00
    EXPECT_EQ(rig.backend->checks(), 1);
    rig.clock->set_utc_now(wizz::make_utc(wizz::Date::from_ymd(2026, 11, 8), 23, 30));  // Sunday
    rig.controller()->evaluate();
    rig.wait_for_evaluations(2);
    EXPECT_EQ(rig.backend->checks(), 1) << "the same week";
    rig.clock->set_utc_now(wizz::make_utc(wizz::Date::from_ymd(2026, 11, 9), 0, 30));  // Monday
    rig.controller()->evaluate();
    rig.wait_for_evaluations(3);
    EXPECT_EQ(rig.backend->checks(), 2) << "a new week";
}

TEST(UpdateController, Turning_checks_on_or_shortening_the_period_checks_at_once_when_due)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::install_on_close, false))};
    rig.start();
    EXPECT_EQ(rig.backend->checks(), 0);
    rig.controller()->set_options(choice(update::WhenFound::install_on_close, true));
    rig.wait_for_evaluations(2);
    EXPECT_EQ(rig.backend->checks(), 1) << "never checked: due as soon as it is on";

    rig.controller()->set_options(choice(update::WhenFound::install_on_close, true, update::CheckFrequency::monthly));
    rig.wait_for_evaluations(3);
    EXPECT_EQ(rig.backend->checks(), 1) << "checked this month";
    rig.controller()->set_options(choice(update::WhenFound::download_then_ask, true, update::CheckFrequency::monthly));
    gui::settle();
    EXPECT_FALSE(rig.controller()->is_running()) << "the install choice does not touch the schedule";
}

TEST(UpdateController, A_new_session_checks_again_for_an_update_seen_earlier)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::notify_only, true, update::CheckFrequency::monthly))};
    rig.backend->schedule().record_success(kNewVersion);  // an earlier session saw 1.2.0 an hour ago
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.start();
    EXPECT_EQ(rig.backend->checks(), 1) << "checked although this month was checked";
    ASSERT_TRUE(rig.bar().isVisible());
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");

    rig.controller()->evaluate();
    rig.wait_for_evaluations(2);
    EXPECT_EQ(rig.backend->checks(), 1) << "once per session";
    EXPECT_EQ(rig.backend->policies().back().recheck_if_newer_than, std::nullopt);
}

TEST(UpdateController, Check_now_reports_checking_then_the_result_and_counts_as_the_periods_check)
{
    UpdateRig rig;
    rig.start();  // 404: up to date
    EXPECT_EQ(rig.controller()->status_view().version_line, "WizzairHelper 1.0.0 · last checked today at 10:00");
    int changes = 0;
    QObject::connect(rig.controller(), &wizz::app::UpdateController::status_changed, rig.controller(),
                     [&changes] { ++changes; });

    rig.clock->advance(std::chrono::minutes{5});
    rig.backend->script_checks({std::unexpected(update_error(UpdateErrorKind::network,
                                                             "Update check failed: network error: no route to host"))});
    rig.controller()->check_now();
    EXPECT_EQ(rig.controller()->status_view().kind, wizz::ui::UpdateStatusKind::checking);
    EXPECT_FALSE(rig.controller()->status_view().check_now_enabled);
    EXPECT_GE(changes, 1);
    wait_idle(rig);
    wizz::ui::UpdateStatusView view = rig.controller()->status_view();
    EXPECT_EQ(view.kind, wizz::ui::UpdateStatusKind::failed);
    EXPECT_EQ(view.status, "Could not reach GitHub. Check your internet connection and try again.");
    EXPECT_EQ(view.version_line, "WizzairHelper 1.0.0 · last checked today at 10:00") << "the last success";
    EXPECT_EQ(update_lines(rig.window_rig).back(), "Update check failed: network error: no route to host");
    EXPECT_FALSE(rig.bar().isVisible());

    rig.clock->advance(std::chrono::minutes{5});
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.controller()->check_now();  // within the failure hour: a manual check still runs
    ASSERT_TRUE(gui::wait_until([&] { return rig.bar().isVisible(); }));
    wait_idle(rig);
    view = rig.controller()->status_view();
    EXPECT_EQ(view.kind, wizz::ui::UpdateStatusKind::ready);
    EXPECT_EQ(view.status, "WizzairHelper 1.2.0 is ready to install.");
    EXPECT_EQ(view.version_line, "WizzairHelper 1.0.0 · last checked today at 10:10");
    EXPECT_EQ(rig.backend->manual_checks(), 2);
    EXPECT_FALSE(rig.backend->schedule().is_check_due()) << "a successful manual check is today's check";
}

TEST(UpdateController, Check_now_during_a_download_runs_after_it)
{
    UpdateRig rig;
    rig.backend->hold_downloads();
    rig.backend->script_checks({release_info(kNewVersion)});
    ASSERT_TRUE(rig.enable());
    rig.window_rig.start();
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->downloads_started() == 1; }));
    gui::settle();
    EXPECT_EQ(rig.controller()->status_view().kind, wizz::ui::UpdateStatusKind::downloading);
    EXPECT_EQ(rig.controller()->status_view().status, "Downloading WizzairHelper 1.2.0…");
    EXPECT_FALSE(rig.controller()->status_view().check_now_enabled);

    rig.controller()->check_now();  // e.g. a click that raced the disabling
    EXPECT_TRUE(rig.controller()->model().checking());
    gui::settle();
    EXPECT_EQ(rig.backend->manual_checks(), 0) << "one piece of work at a time";
    rig.backend->release_downloads();
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->manual_checks() == 1; })) << "queued, then run";
    wait_idle(rig);
    EXPECT_EQ(rig.controller()->status_view().kind, wizz::ui::UpdateStatusKind::ready);
}

TEST(UpdateController, An_unexpected_failure_of_Check_now_never_stays_busy)
{
    UpdateRig rig;
    rig.start();
    rig.backend->set_manual_checks_throw(true);
    rig.controller()->check_now();
    wait_idle(rig);
    const wizz::ui::UpdateStatusView view = rig.controller()->status_view();
    EXPECT_EQ(view.kind, wizz::ui::UpdateStatusKind::failed);
    EXPECT_EQ(view.status, "Something went wrong: scripted failure of the check");
    EXPECT_TRUE(view.check_now_enabled);
    EXPECT_EQ(update_lines(rig.window_rig).back(), "Update check failed: scripted failure of the check");
    EXPECT_TRUE(rig.window_rig.dialogs.errors.empty()) << "never a crash box";
}

TEST(UpdateController, Download_then_ask_offers_restart_but_closing_does_not_install)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::download_then_ask))};
    reach_ready(rig);
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is ready to install.");
    EXPECT_EQ(rig.bar().action_button()->text(), "Restart and update");
    rig.window().close();
    EXPECT_TRUE(rig.launches.commands.empty()) << "only Restart and update installs";
    EXPECT_EQ(rig.launches.mutex_releases, 0);
}

TEST(UpdateController, Download_then_ask_restart_and_update_installs_with_RELAUNCH)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::download_then_ask))};
    reach_ready(rig);
    rig.bar().action_button()->click();
    EXPECT_FALSE(rig.window().isVisible());
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_EQ(rig.launches.commands.front().command_line(), expected_command_line(rig, true));
}

TEST(UpdateController, Notify_only_tells_and_Update_now_downloads_then_restarts_and_installs)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::notify_only))};
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.start();
    ASSERT_TRUE(rig.bar().isVisible());
    EXPECT_TRUE(rig.backend->downloaded().empty()) << "no download until the user asks";
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(rig.bar().action_button()->text(), "Update now");
    EXPECT_EQ(rig.controller()->status_view().kind, wizz::ui::UpdateStatusKind::available);

    rig.backend->hold_downloads();
    rig.bar().action_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->downloads_started() == 1; }));
    gui::settle();
    EXPECT_EQ(rig.bar().message_label()->text(), q("Downloading WizzairHelper 1.2.0…"));
    EXPECT_FALSE(rig.bar().action_button()->isVisible()) << "no action while it downloads";
    EXPECT_TRUE(rig.bar().later_button()->isVisible());
    EXPECT_TRUE(rig.window().isVisible());

    rig.backend->release_downloads();
    ASSERT_TRUE(gui::wait_until([&] { return !rig.window().isVisible(); })) << "verified: the app restarts";
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_EQ(rig.launches.commands.front().command_line(), expected_command_line(rig, true));
    EXPECT_EQ(rig.launches.released_before_launch, std::vector<bool>{true});
}

TEST(UpdateController, Notify_only_closing_installs_nothing)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::notify_only))};
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.start();
    ASSERT_TRUE(rig.bar().isVisible());
    rig.window().close();
    EXPECT_TRUE(rig.launches.commands.empty());
    EXPECT_TRUE(rig.backend->downloaded().empty());
}

TEST(UpdateController, Switching_to_install_on_close_with_a_verified_download_installs_it_on_close)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::download_then_ask))};
    reach_ready(rig);
    rig.controller()->set_options(choice(update::WhenFound::install_on_close));
    EXPECT_EQ(rig.bar().message_label()->text(), q(kReadyText));
    rig.window().close();
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_EQ(rig.launches.commands.front().command_line(), expected_command_line(rig, false));
}

TEST(UpdateController, Switching_from_notify_only_to_a_downloading_choice_downloads_at_once)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::notify_only))};
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.start();
    ASSERT_TRUE(rig.backend->downloaded().empty());
    rig.controller()->set_options(choice(update::WhenFound::install_on_close));
    ASSERT_TRUE(gui::wait_until([&] { return rig.controller()->model().ready().has_value(); }));
    wait_idle(rig);
    EXPECT_EQ(rig.backend->downloaded(), std::vector<Version>{kNewVersion});
    EXPECT_EQ(rig.bar().message_label()->text(), q(kReadyText));
    rig.window().close();
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_EQ(rig.launches.commands.front().command_line(), expected_command_line(rig, false));
}

TEST(UpdateController, A_portable_copy_shows_the_install_choices_disabled_with_a_reason)
{
    UpdateRigOptions options = with_updates(choice(update::WhenFound::download_then_ask), InstallKind::portable);
    UpdateRig rig{std::move(options)};
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.start();
    const wizz::ui::UpdateStatusView view = rig.controller()->status_view();
    EXPECT_FALSE(view.when_found_enabled);
    EXPECT_EQ(view.when_found_note,
              "This portable copy cannot install updates itself; it links to the release page instead.");
    EXPECT_TRUE(view.options_enabled);
    EXPECT_EQ(view.kind, wizz::ui::UpdateStatusKind::available);
    EXPECT_EQ(rig.bar().action_button()->text(), "Download");
    EXPECT_TRUE(rig.backend->downloaded().empty());
}

// ---------------------------------------------------------------------------
// The real core behind the controller
// ---------------------------------------------------------------------------

TEST(UpdateController, The_real_core_checks_downloads_and_verifies_through_the_transport)
{
    const std::string installer = "MZ fake installer of 1.2.0";
    const std::string sums = std::format("{}  WizzairHelper-1.2.0-win64-setup.exe\n", update::sha256_hex(installer));
    const auto asset = [](std::string_view name, int id) {
        return std::format(R"({{"url":"https://api.github.com/repos/Giorgi-Apriashvili/WizzairHelper/releases/assets/{}",)"
                           R"("id":{},"name":"{}","state":"uploaded","size":10,)"
                           R"("browser_download_url":"https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/download/v1.2.0/{}"}})",
                           id, id, name, name);
    };
    const std::string release = std::format(
        R"({{"html_url":"{}","tag_name":"v1.2.0","draft":false,"prerelease":false,"assets":[{},{},{}]}})", kReleasePage,
        asset("WizzairHelper-1.2.0-win64-setup.exe", 1), asset("WizzairHelper-1.2.0-win64.zip", 2),
        asset("SHA256SUMS.txt", 3));
    const auto transport = std::make_shared<wizz::test::FakeTransport>([&](const wizz::test::RecordedRequest& request) {
        const auto bytes = [](std::string body) {
            return wizz::test::ScriptedResponse{.response = wizz::http::Response{.status = 200,
                                                                                 .headers = {},
                                                                                 .content_type = "application/octet-stream",
                                                                                 .body = std::move(body)},
                                                .body_error = std::nullopt};
        };
        if (request.url.ends_with("/releases/latest")) {
            return wizz::test::json_response(release);
        }
        if (request.url.contains("SHA256SUMS.txt")) {
            return bytes(sums);
        }
        if (request.url.contains("-win64-setup.exe")) {
            return bytes(installer);
        }
        return wizz::test::json_response("{}", 404);
    });

    WindowRig rig;
    const auto clock = std::make_shared<wizz::FixedClock>();
    const update::UpdateConfig config = update::UpdateConfig::for_app("1.0.0", rig.data_root());
    auto backend = std::make_shared<wizz::app::CoreUpdateBackend>(
        config, InstallKind::installed, transport, clock,
        [](wizz::Duration, std::stop_token stop) { wizz::throw_if_cancelled(stop); });
    EXPECT_EQ(backend->running_version(), kRunningVersion);
    wizz::test::app::LaunchLog launches;
    ASSERT_TRUE(rig.window().enable_updates(launches.hooks(backend)));
    rig.start();
    ASSERT_TRUE(gui::wait_until([&] { return rig.window().update_bar()->isVisible(); }));

    EXPECT_EQ(rig.window().update_bar()->message_label()->text(), q(kReadyText));
    const std::filesystem::path downloaded = config.download_dir() / "WizzairHelper-1.2.0-win64-setup.exe";
    EXPECT_TRUE(std::filesystem::exists(downloaded));
    EXPECT_TRUE(std::filesystem::exists(config.state_file()));
    EXPECT_TRUE(std::filesystem::exists(config.log_file()));
    EXPECT_FALSE(std::filesystem::exists(rig.data_root() / "update.log")) << "nothing of the updater in the data root";
    const std::vector<std::string> urls = transport->urls();
    ASSERT_EQ(urls.size(), 3u);
    EXPECT_EQ(urls[0], "https://api.github.com/repos/Giorgi-Apriashvili/WizzairHelper/releases/latest");
    EXPECT_TRUE(urls[1].contains("SHA256SUMS.txt"));
    EXPECT_TRUE(urls[2].contains("WizzairHelper-1.2.0-win64-setup.exe"));
    EXPECT_EQ(transport->requests()[0].header_values("User-Agent"),
              std::vector<std::string>{"WizzairHelper/1.0.0 (update check)"});

    rig.window().close();
    ASSERT_EQ(launches.commands.size(), 1u);
    EXPECT_EQ(launches.commands.front().program, downloaded);
}

TEST(UpdateController, The_system_hooks_use_the_running_version_and_the_updates_folder)
{
    WindowRig rig;
    bool released = false;
    const wizz::app::UpdateHooks hooks =
        wizz::app::UpdateHooks::system(rig.services(), [&released] { released = true; });
    ASSERT_NE(hooks.backend, nullptr);
    EXPECT_EQ(hooks.backend->running_version().to_string(), WIZZ_VERSION_STRING);
    EXPECT_EQ(hooks.backend->install_kind(), InstallKind::portable) << "the test binary has no unins000.exe";
    EXPECT_TRUE(hooks.launch_installer);
    EXPECT_TRUE(hooks.open_release_page);
    EXPECT_EQ(hooks.recheck_interval, wizz::app::kUpdateRecheckInterval);
    ASSERT_TRUE(hooks.release_app_mutex);
    hooks.release_app_mutex();
    EXPECT_TRUE(released);
    const auto* core = dynamic_cast<const wizz::app::CoreUpdateBackend*>(hooks.backend.get());
    ASSERT_NE(core, nullptr);
    EXPECT_EQ(core->config().updates_dir(), rig.data_root() / "updates");
}

} // namespace
