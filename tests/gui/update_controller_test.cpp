// The self-updater in the main window (D-45..D-49): it checks on a worker after the
// window is shown when the core says a check is due, re-evaluates every hour, never
// runs in self-test runs, stays silent apart from Log lines, downloads the installer
// of an installed copy before offering "Restart and update", links a portable copy
// to the release page, and starts the silent installer when the window closes —
// through a recording launcher, never a real process. An update put off in an earlier
// session is offered again at every start without any request (the backend counts them).

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

TEST(UpdateController, An_update_seen_in_an_earlier_session_is_offered_again_but_checked_only_in_the_next_period)
{
    // The owner's rules: the period alone decides when GitHub is asked (D-59), and an
    // update put off earlier is offered again at every start from what the app knows
    // (D-61). An earlier session of this month saw 1.2.0 and did not install it.
    UpdateRig rig{with_updates(choice(update::WhenFound::notify_only, true, update::CheckFrequency::monthly))};
    rig.backend->schedule().record_success(kNewVersion);
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.clock->advance(std::chrono::days{3});
    rig.start();
    EXPECT_EQ(rig.backend->requests(), 0) << "this month was checked: no extra request";
    ASSERT_TRUE(rig.bar().isVisible()) << "but the update is offered again";
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(rig.bar().action_button()->text(), "Update now");
    EXPECT_EQ(rig.controller()->status_view().status, "WizzairHelper 1.2.0 is available.");

    rig.clock->set_utc_now(wizz::make_utc(wizz::Date::from_ymd(2026, 12, 1), 8, 0));
    rig.controller()->evaluate();
    rig.wait_for_evaluations(2);
    EXPECT_EQ(rig.backend->checks(), 1) << "a new month checks";
    ASSERT_TRUE(rig.bar().isVisible());
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(update_lines(rig.window_rig),
              std::vector<std::string>{"Update available: WizzairHelper 1.2.0 (running 1.0.0)."})
        << "announced once in the session";
}

// ---------------------------------------------------------------------------
// The reminder at start-up: an update put off in an earlier session (D-61)
// ---------------------------------------------------------------------------

/// An earlier session today saw 1.2.0 (no check is due today) and did not install it;
/// with `installer` its verified installer is still in the downloads folder.
void put_off_earlier(UpdateRig& rig, bool installer)
{
    rig.backend->schedule().record_success(kNewVersion);
    if (installer) {
        rig.backend->place_verified_installer(kNewVersion);
    }
    rig.backend->script_checks({release_info(kNewVersion)});  // answered only if something asks
}

TEST(UpdateController, Reminds_at_start_of_a_verified_installer_as_ready_without_any_request)
{
    for (const update::WhenFound when_found : {update::WhenFound::install_on_close, update::WhenFound::download_then_ask}) {
        SCOPED_TRACE(static_cast<int>(when_found));
        UpdateRig rig{with_updates(choice(when_found))};
        put_off_earlier(rig, true);
        rig.start();
        EXPECT_EQ(rig.backend->evaluations(), 1) << "the schedule was asked";
        EXPECT_EQ(rig.backend->requests(), 0) << "no check is due, and the reminder asks nothing";
        EXPECT_EQ(rig.backend->cleanups(), 1) << "old downloads are still cleaned up";
        ASSERT_TRUE(rig.bar().isVisible());
        const bool on_close = when_found == update::WhenFound::install_on_close;
        EXPECT_EQ(rig.bar().message_label()->text(),
                  on_close ? q(kReadyText) : QStringLiteral("WizzairHelper 1.2.0 is ready to install."));
        EXPECT_EQ(rig.bar().action_button()->text(), "Restart and update");
        EXPECT_TRUE(rig.bar().action_button()->isEnabled());
        EXPECT_EQ(rig.controller()->status_view().status, "WizzairHelper 1.2.0 is ready to install.");
        EXPECT_EQ(update_lines(rig.window_rig),
                  std::vector<std::string>{"Update available: WizzairHelper 1.2.0 (running 1.0.0)."});

        rig.window().close();
        EXPECT_EQ(rig.backend->requests(), 0);
        if (on_close) {
            ASSERT_EQ(rig.launches.commands.size(), 1u) << "installed on close, as today";
            EXPECT_EQ(rig.launches.commands.front().command_line(), expected_command_line(rig, false));
            EXPECT_EQ(rig.launches.released_before_launch, std::vector<bool>{true});
        } else {
            EXPECT_TRUE(rig.launches.commands.empty()) << "download then ask: only Restart and update installs";
        }
    }
}

TEST(UpdateController, Restart_and_update_on_a_reminder_installs_without_any_request)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::download_then_ask))};
    put_off_earlier(rig, true);
    rig.start();
    ASSERT_TRUE(rig.bar().isVisible());
    rig.bar().action_button()->click();
    EXPECT_FALSE(rig.window().isVisible());
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_EQ(rig.launches.commands.front().command_line(), expected_command_line(rig, true));
    EXPECT_EQ(rig.backend->requests(), 0);
}

TEST(UpdateController, Reminds_at_start_with_Update_now_without_a_verified_installer_or_under_notify_only)
{
    struct Case {
        update::WhenFound when_found;
        bool automatic;
        bool installer;
    };
    for (const Case c : {Case{update::WhenFound::install_on_close, true, false},
                         Case{update::WhenFound::download_then_ask, true, false},
                         Case{update::WhenFound::notify_only, true, false}, Case{update::WhenFound::notify_only, true, true},
                         Case{update::WhenFound::install_on_close, false, false}}) {
        SCOPED_TRACE(std::format("choice {}, automatic {}, installer {}", static_cast<int>(c.when_found), c.automatic,
                                 c.installer));
        UpdateRig rig{with_updates(choice(c.when_found, c.automatic))};
        put_off_earlier(rig, c.installer);
        rig.start();
        EXPECT_EQ(rig.backend->requests(), 0) << "the reminder asks nothing";
        ASSERT_TRUE(rig.bar().isVisible());
        EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");
        EXPECT_EQ(rig.bar().action_button()->text(), "Update now");
        EXPECT_TRUE(rig.bar().action_button()->isEnabled());
        EXPECT_EQ(rig.controller()->status_view().kind, wizz::ui::UpdateStatusKind::available);

        rig.controller()->evaluate();  // an hourly tick: still nothing is due, nothing is fetched
        rig.wait_for_evaluations(2);
        EXPECT_EQ(rig.backend->requests(), 0);
        rig.window().close();
        EXPECT_TRUE(rig.launches.commands.empty()) << "closing installs nothing";
        EXPECT_EQ(rig.backend->requests(), 0);
    }
}

TEST(UpdateController, Update_now_on_a_reminder_checks_for_the_release_then_downloads_and_restarts)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::notify_only))};
    put_off_earlier(rig, true);
    rig.start();
    ASSERT_EQ(rig.bar().action_button()->text(), "Update now");
    ASSERT_EQ(rig.backend->requests(), 0);

    rig.backend->hold_downloads();
    rig.bar().action_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->downloads_started() == 1; }));
    gui::settle();
    EXPECT_EQ(rig.backend->manual_checks(), 1) << "the user asked: one check for its release";
    EXPECT_EQ(rig.bar().message_label()->text(), q("Downloading WizzairHelper 1.2.0…"));
    EXPECT_FALSE(rig.bar().action_button()->isVisible());
    EXPECT_EQ(rig.controller()->status_view().status, "Downloading WizzairHelper 1.2.0…");

    rig.backend->release_downloads();
    ASSERT_TRUE(gui::wait_until([&] { return !rig.window().isVisible(); })) << "verified: the app restarts";
    EXPECT_EQ(rig.backend->downloaded(), std::vector<Version>{kNewVersion});
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_EQ(rig.launches.commands.front().command_line(), expected_command_line(rig, true));
    EXPECT_EQ(rig.backend->checks(), 1) << "one check, one download";
}

TEST(UpdateController, Update_now_on_a_reminder_offers_itself_again_when_its_check_fails)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::install_on_close))};
    put_off_earlier(rig, false);
    rig.backend->script_checks({std::unexpected(update_error(UpdateErrorKind::network,
                                                             "Update check failed: network error: no route to host"))});
    rig.start();
    ASSERT_EQ(rig.bar().action_button()->text(), "Update now");
    rig.clock->advance(std::chrono::minutes{10});
    rig.bar().action_button()->click();
    wait_idle(rig);
    EXPECT_EQ(rig.backend->checks(), 1);
    EXPECT_TRUE(rig.backend->downloaded().empty());
    ASSERT_TRUE(rig.bar().isVisible());
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(rig.bar().action_button()->text(), "Update now") << "offered again";
    const wizz::ui::UpdateStatusView view = rig.controller()->status_view();
    EXPECT_EQ(view.kind, wizz::ui::UpdateStatusKind::failed);
    EXPECT_EQ(view.status, "Could not reach GitHub. Check your internet connection and try again.");
    EXPECT_EQ(view.version_line, "WizzairHelper 1.0.0 · last successful check today at 10:00");
    EXPECT_EQ(update_lines(rig.window_rig).back(), "Update check failed: network error: no route to host");
    EXPECT_TRUE(rig.window().isVisible());
}

TEST(UpdateController, A_portable_copy_is_reminded_with_its_release_page_without_any_request)
{
    UpdateRig rig{with_updates(choice(update::WhenFound::install_on_close), InstallKind::portable)};
    put_off_earlier(rig, false);
    rig.start();
    ASSERT_TRUE(rig.bar().isVisible());
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(rig.bar().action_button()->text(), "Download");
    rig.bar().action_button()->click();
    EXPECT_EQ(rig.launches.opened_pages, std::vector<std::string>{std::string{kReleasePage}});
    EXPECT_EQ(rig.backend->requests(), 0) << "the page is opened in the browser; the app sends nothing";
    rig.window().close();
    EXPECT_TRUE(rig.launches.commands.empty());
}

TEST(UpdateController, No_reminder_for_a_version_that_is_not_newer)
{
    for (const Version seen : {kRunningVersion, Version{0, 9, 0}}) {
        SCOPED_TRACE(seen.to_string());
        UpdateRig rig;
        rig.backend->schedule().record_success(seen);
        rig.backend->place_verified_installer(seen);
        rig.start();
        EXPECT_EQ(rig.backend->requests(), 0);
        EXPECT_EQ(rig.backend->cleanups(), 1) << "old downloads are cleaned up as today";
        EXPECT_FALSE(rig.bar().isVisible());
        EXPECT_TRUE(update_lines(rig.window_rig).empty());
        EXPECT_EQ(rig.controller()->status_view().kind, wizz::ui::UpdateStatusKind::up_to_date);
        rig.window().close();
        EXPECT_TRUE(rig.launches.commands.empty()) << "never an installer of the running or an older version";
    }
}

TEST(UpdateController, Later_hides_the_reminder_until_the_next_start)
{
    UpdateRigOptions options = with_updates(choice(update::WhenFound::notify_only));
    options.interval = 20ms;  // the hours of the session, compressed
    UpdateRig first{std::move(options)};
    put_off_earlier(first, false);
    first.start();
    ASSERT_TRUE(first.bar().isVisible());
    first.bar().later_button()->click();
    EXPECT_FALSE(first.bar().isVisible());
    first.wait_for_evaluations(4);
    EXPECT_FALSE(first.bar().isVisible()) << "hidden for the rest of the session";
    EXPECT_EQ(first.backend->requests(), 0);
    first.window().close();

    // The next start, with the files the first session left: offered again, still without a request.
    UpdateRig next{with_updates(choice(update::WhenFound::notify_only))};
    std::filesystem::copy(first.window_rig.data_root() / "updates", next.window_rig.data_root() / "updates",
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    next.start();
    ASSERT_TRUE(next.bar().isVisible());
    EXPECT_EQ(next.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(next.backend->requests(), 0);
}

TEST(UpdateController, The_first_evaluation_reminds_also_after_a_Check_now_that_was_running)
{
    // Check now ran before the window was shown; the first evaluation waits for it instead
    // of being skipped, so old downloads still go and the reminder still comes. A failed
    // check knows nothing new, so the update put off earlier stands.
    UpdateRig rig;
    put_off_earlier(rig, true);
    rig.backend->script_checks({std::unexpected(update_error(UpdateErrorKind::network,
                                                             "Update check failed: network error: no route to host"))});
    rig.backend->hold_checks();
    ASSERT_TRUE(rig.enable());
    rig.controller()->check_now();
    rig.window_rig.start();  // shown while Check now still runs
    ASSERT_TRUE(rig.controller()->is_started());
    ASSERT_TRUE(rig.controller()->is_running());
    EXPECT_EQ(rig.backend->evaluations(), 0) << "one piece of work at a time";
    rig.backend->release_checks();
    wait_idle(rig);
    EXPECT_EQ(rig.backend->manual_checks(), 1);
    EXPECT_EQ(rig.backend->requests(), 1) << "Check now only";
    EXPECT_EQ(rig.backend->evaluations(), 1);
    EXPECT_EQ(rig.backend->cleanups(), 1) << "the first evaluation ran after it";
    ASSERT_TRUE(rig.bar().isVisible());
    EXPECT_EQ(rig.bar().message_label()->text(), q(kReadyText));
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
    EXPECT_EQ(view.version_line, "WizzairHelper 1.0.0 · last successful check today at 10:00")
        << "the last success, named as such above the failure";
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

TEST(UpdateController, A_first_check_that_fails_has_no_successful_check_yet)
{
    UpdateRig rig;
    rig.backend->script_checks({std::unexpected(update_error(UpdateErrorKind::http_status,
                                                             "Update check failed: GitHub answered HTTP 503."))});
    ASSERT_TRUE(rig.enable());
    EXPECT_EQ(rig.controller()->status_view().version_line, "WizzairHelper 1.0.0 · never checked");
    rig.window_rig.start();
    rig.wait_for_evaluations(1);
    const wizz::ui::UpdateStatusView view = rig.controller()->status_view();
    EXPECT_EQ(view.kind, wizz::ui::UpdateStatusKind::failed);
    EXPECT_EQ(view.version_line, "WizzairHelper 1.0.0 · no successful check yet");
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

TEST(UpdateController, Runs_one_piece_of_work_at_a_time_and_queues_the_rest)
{
    // While a download is held on its worker, nothing else may start a job: Check now, an
    // hourly tick and an option change that makes a check due all wait. live_jobs() counts
    // the jobs the controller started synchronously, so a second job would show at once.
    UpdateRig rig{with_updates(choice(update::WhenFound::install_on_close, true, update::CheckFrequency::monthly))};
    rig.backend->hold_downloads();
    rig.backend->script_checks({release_info(kNewVersion)});
    ASSERT_TRUE(rig.enable());
    rig.window_rig.start();
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->downloads_started() == 1; }));
    gui::settle();
    ASSERT_TRUE(rig.controller()->is_running());
    EXPECT_EQ(rig.controller()->live_jobs(), 1u);

    rig.controller()->check_now();
    EXPECT_EQ(rig.controller()->live_jobs(), 1u) << "Check now waits for the download";
    rig.controller()->evaluate();
    EXPECT_EQ(rig.controller()->live_jobs(), 1u) << "a tick during work is skipped";
    rig.clock->advance(std::chrono::days{1});
    rig.controller()->set_options(choice(update::WhenFound::install_on_close, true, update::CheckFrequency::daily));
    EXPECT_EQ(rig.controller()->live_jobs(), 1u) << "a check that became due waits too";
    gui::settle();
    EXPECT_EQ(rig.controller()->live_jobs(), 1u);
    EXPECT_EQ(rig.backend->manual_checks(), 0);
    const int evaluations = rig.backend->evaluations();

    rig.backend->release_downloads();
    wait_idle(rig);
    EXPECT_EQ(rig.backend->manual_checks(), 1) << "queued, then run";
    EXPECT_EQ(rig.backend->evaluations(), evaluations + 1) << "the queued evaluation, once";
    EXPECT_EQ(rig.controller()->live_jobs(), 0u);
    EXPECT_EQ(rig.controller()->status_view().kind, wizz::ui::UpdateStatusKind::ready);
}

TEST(UpdateController, A_queued_download_is_dropped_when_the_choice_goes_back_to_notify_only)
{
    // Only tell me, with 1.2.0 offered. While Check now runs, the user picks a choice that
    // downloads (the download queues behind the check) and then goes back: nothing is
    // downloaded, and Update now is offered as before.
    UpdateRig rig{with_updates(choice(update::WhenFound::notify_only))};
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.start();
    ASSERT_EQ(rig.bar().action_button()->text(), "Update now");

    rig.controller()->check_now();
    ASSERT_TRUE(rig.controller()->is_running());
    rig.controller()->set_options(choice(update::WhenFound::install_on_close));
    rig.controller()->set_options(choice(update::WhenFound::notify_only));
    wait_idle(rig);
    EXPECT_TRUE(rig.backend->downloaded().empty()) << "the saved choice only tells";
    EXPECT_EQ(rig.backend->downloads_started(), 0);
    ASSERT_TRUE(rig.bar().isVisible());
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(rig.bar().action_button()->text(), "Update now");

    // The same with automatic checks switched off before the queued download starts, this
    // time behind an hourly evaluation (nothing is due: checked today).
    rig.controller()->evaluate();
    ASSERT_TRUE(rig.controller()->is_running());
    rig.controller()->set_options(choice(update::WhenFound::download_then_ask));
    rig.controller()->set_options(choice(update::WhenFound::download_then_ask, false));
    wait_idle(rig);
    EXPECT_TRUE(rig.backend->downloaded().empty()) << "no request on its own while checks are off";
    EXPECT_EQ(rig.bar().action_button()->text(), "Update now") << "the user can still ask for it";
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

TEST(UpdateController, The_real_core_reminds_from_its_files_without_a_request)
{
    // update-state.json of an earlier session that saw 1.2.0 today, its verified installer
    // and a stale one of an older version in the downloads folder; the transport answers
    // every request with HTTP 500 and records it.
    const auto transport = std::make_shared<wizz::test::FakeTransport>(
        [](const wizz::test::RecordedRequest&) { return wizz::test::json_response("{}", 500); });
    WindowRig rig;
    const auto clock = std::make_shared<wizz::FixedClock>();
    const update::UpdateConfig config = update::UpdateConfig::for_app("1.0.0", rig.data_root());
    update::UpdateSchedule{config.updates_dir(), clock}.record_success(kNewVersion);
    const std::filesystem::path installer = config.download_dir() / "WizzairHelper-1.2.0-win64-setup.exe";
    const std::filesystem::path stale = config.download_dir() / "WizzairHelper-0.9.0-win64-setup.exe";
    std::filesystem::create_directories(config.download_dir());
    std::ofstream{installer, std::ios::binary} << "MZ verified by an earlier session";
    std::ofstream{stale, std::ios::binary} << "MZ";
    auto backend = std::make_shared<wizz::app::CoreUpdateBackend>(
        config, InstallKind::installed, transport, clock,
        [](wizz::Duration, std::stop_token stop) { wizz::throw_if_cancelled(stop); });
    wizz::test::app::LaunchLog launches;
    ASSERT_TRUE(rig.window().enable_updates(launches.hooks(backend)));
    rig.start();
    ASSERT_TRUE(gui::wait_until([&] { return rig.window().update_bar()->isVisible(); }));

    EXPECT_EQ(rig.window().update_bar()->message_label()->text(), q(kReadyText));
    EXPECT_TRUE(transport->urls().empty()) << "nothing was sent to GitHub";
    EXPECT_FALSE(std::filesystem::exists(stale)) << "old downloads are still cleaned up";
    rig.window().close();
    ASSERT_EQ(launches.commands.size(), 1u);
    EXPECT_EQ(launches.commands.front().program, installer);
    EXPECT_TRUE(transport->urls().empty());
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
