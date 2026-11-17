// The self-updater's presentation rules (DECISIONS D-45..D-49, D-59..D-62): when the
// app may check, the bar texts and buttons, the Log tab lines, what a check or download
// result means for an installed or a portable copy under each "When an update is found"
// choice, "Later", "Restart and update", "Update now", changing the options while an
// update is under way, the installer command run on exit, and the Settings window's
// "Updates" status (the version line says "last successful check" after a failed check).

#include "wizz/ui/update_model.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/core/locale.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace update = wizz::update;
using update::CheckFrequency;
using update::InstallKind;
using update::UpdateErrorKind;
using update::UpdateOptions;
using update::Version;
using update::WhenFound;
using wizz::ui::BarAction;
using wizz::ui::CheckOrigin;
using wizz::ui::UpdateBarView;
using wizz::ui::UpdateModel;
using wizz::ui::UpdateStatusKind;
using wizz::ui::UpdateStatusView;
using wizz::ui::updates_allowed;
namespace texts = wizz::ui::update_strings;

const Version kRunning{1, 0, 0};
const Version kNew{1, 2, 0};
const std::filesystem::path kDownloads{L"C:\\Users\\Anna Smith\\AppData\\Local\\WizzairHelper\\updates\\downloads"};

[[nodiscard]] update::ReleaseAsset asset(const std::string& name)
{
    return update::ReleaseAsset{
        .name = name,
        .browser_download_url = "https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/download/x/" + name,
        .api_url = "https://api.github.com/repos/Giorgi-Apriashvili/WizzairHelper/releases/assets/1",
        .size = 100};
}

[[nodiscard]] update::ReleaseInfo release(Version version, bool newer = true)
{
    const std::string text = version.to_string();
    return update::ReleaseInfo{
        .tag = "v" + text,
        .version = version,
        .html_url = "https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v" + text,
        .newer = newer,
        .installer = asset("WizzairHelper-" + text + "-win64-setup.exe"),
        .portable_zip = asset("WizzairHelper-" + text + "-win64.zip"),
        .checksums = asset("SHA256SUMS.txt")};
}

[[nodiscard]] wizz::ui::CheckResult found(Version version, bool newer = true)
{
    return release(version, newer);
}

[[nodiscard]] update::UpdateError error(UpdateErrorKind kind, std::string message, std::optional<int> status = {})
{
    return update::UpdateError{.kind = kind, .message = std::move(message), .http_status = status, .tries = {}};
}

[[nodiscard]] wizz::ui::CheckResult check_error(UpdateErrorKind kind, std::string message,
                                                std::optional<int> status = {})
{
    return std::unexpected(error(kind, std::move(message), status));
}

[[nodiscard]] std::filesystem::path installer_path(Version version)
{
    return kDownloads / ("WizzairHelper-" + version.to_string() + "-win64-setup.exe");
}

[[nodiscard]] wizz::ui::DownloadResult downloaded(Version version)
{
    return update::DownloadedFile{.path = installer_path(version),
                                  .sha256 = std::string(64, 'a'),
                                  .reused = false,
                                  .tries = {}};
}

[[nodiscard]] wizz::ui::DownloadResult download_error(UpdateErrorKind kind, std::string message,
                                                      std::optional<int> status = {})
{
    return std::unexpected(error(kind, std::move(message), status));
}

[[nodiscard]] std::vector<std::string> lines(std::initializer_list<std::string> list)
{
    return std::vector<std::string>{list};
}

[[nodiscard]] UpdateOptions with(WhenFound when_found, bool automatic = true,
                                 CheckFrequency frequency = CheckFrequency::daily)
{
    return UpdateOptions{.check_automatically = automatic, .check_frequency = frequency, .when_found = when_found};
}

[[nodiscard]] std::vector<std::string> relaunch_args(bool relaunch)
{
    std::vector<std::string> args{"/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS"};
    if (relaunch) {
        args.emplace_back("/RELAUNCH");
    }
    return args;
}

/// The model's download as the controller runs it: begin, then the result.
UpdateModel::DownloadOutcome run_download(UpdateModel& model, const wizz::ui::DownloadResult& result,
                                          bool working = false)
{
    model.begin_download();
    return model.on_download(result, working);
}

/// 2026-11-17 15:53 UTC (a Tuesday), the local zone UTC.
[[nodiscard]] wizz::FixedClock settings_clock(std::chrono::hours offset = 0h)
{
    return wizz::FixedClock{wizz::make_utc(wizz::Date::from_ymd(2026, 11, 17), 15, 53), offset};
}

[[nodiscard]] UpdateStatusView status(const UpdateModel& model)
{
    return model.status_view(settings_clock(), *wizz::Locale::en_us());
}

// =====================================================================================
// Rules and texts
// =====================================================================================

TEST(UpdateRules, Self_test_and_replay_runs_never_check)
{
    EXPECT_TRUE(updates_allowed(std::vector<std::string>{}));
    EXPECT_TRUE(updates_allowed(std::vector<std::string>{"--keep-open", "--diagnostics"}));
    EXPECT_FALSE(updates_allowed(std::vector<std::string>{"--autotest", "out"}));
    EXPECT_FALSE(updates_allowed(std::vector<std::string>{"--autotest"})) << "even without its value";
    EXPECT_FALSE(updates_allowed(std::vector<std::string>{"--replay", "tests\\replay\\named_destination"}));
    EXPECT_FALSE(updates_allowed(std::vector<std::string>{"--from", "Graz", "--replay", "dir"}));
    EXPECT_TRUE(updates_allowed(std::vector<std::string>{"--Autotest", "--REPLAY", "-replay"}))
        << "exact, case-sensitive flags (01 \u00A717.1)";
}

TEST(UpdateRules, Bar_texts_and_log_lines)
{
    EXPECT_EQ(texts::kRestartAndUpdate, "Restart and update");
    EXPECT_EQ(texts::kLater, "Later");
    EXPECT_EQ(texts::kDownload, "Download");
    EXPECT_EQ(texts::kUpdateNow, "Update now");
    EXPECT_EQ(texts::ready_text(Version{1, 2, 0}),
              "WizzairHelper 1.2.0 is ready to install. It will be installed when you close WizzairHelper.");
    EXPECT_EQ(texts::ready_to_install_text(Version{1, 2, 0}), "WizzairHelper 1.2.0 is ready to install.");
    EXPECT_EQ(texts::available_text(Version{1, 10, 3}), "WizzairHelper 1.10.3 is available.");
    EXPECT_EQ(texts::downloading_text(Version{1, 2, 0}), "Downloading WizzairHelper 1.2.0\u2026");

    EXPECT_EQ(texts::available_log(Version{1, 2, 0}, Version{1, 0, 0}),
              "Update available: WizzairHelper 1.2.0 (running 1.0.0).");
    EXPECT_EQ(texts::verified_log(Version{1, 2, 0}), "Update 1.2.0 downloaded and verified.");
    EXPECT_EQ(texts::install_started_log(Version{1, 2, 0}), "Update 1.2.0: installer started.");
    EXPECT_EQ(texts::install_failed_log(Version{1, 2, 0}, "Access is denied."),
              "Update 1.2.0: the installer could not be started: Access is denied.");
    EXPECT_EQ(texts::download_failed_log("Could not download WizzairHelper-1.2.0-win64-setup.exe after 5 tries: HTTP 503."),
              "Update download failed: Could not download WizzairHelper-1.2.0-win64-setup.exe after 5 tries: HTTP 503.");

    // The core's own check messages start with "Update check failed: "; it is not doubled.
    EXPECT_EQ(texts::check_failed_log("Update check failed: GitHub answered HTTP 500."),
              "Update check failed: GitHub answered HTTP 500.");
    EXPECT_EQ(texts::check_failed_log("Update check failed: network error: Could not resolve host: api.github.com"),
              "Update check failed: network error: Could not resolve host: api.github.com");
    EXPECT_EQ(texts::check_failed_log("The latest release could not be read: the tag \"v1.2\" is not vX.Y.Z."),
              "Update check failed: The latest release could not be read: the tag \"v1.2\" is not vX.Y.Z.");
}

TEST(UpdateRules, Settings_window_texts)
{
    EXPECT_EQ(texts::kSettingsTitle, "Settings");
    EXPECT_EQ(texts::kUpdatesSection, "Updates");
    EXPECT_EQ(texts::kCheckNow, "Check now");
    EXPECT_EQ(texts::kClose, "Close");
    EXPECT_EQ(texts::kCheckAutomatically, "Check for updates automatically");
    EXPECT_EQ(texts::kWhenFoundCaption, "When an update is found:");
    EXPECT_EQ(texts::frequency_label(CheckFrequency::daily), "Once a day");
    EXPECT_EQ(texts::frequency_label(CheckFrequency::weekly), "Once a week");
    EXPECT_EQ(texts::frequency_label(CheckFrequency::monthly), "Once a month");
    EXPECT_EQ(texts::when_found_label(WhenFound::install_on_close), "Download it and install when I close the app");
    EXPECT_EQ(texts::when_found_label(WhenFound::download_then_ask), "Download it, then ask me before installing");
    EXPECT_EQ(texts::when_found_label(WhenFound::notify_only), "Only tell me that an update is available");
    EXPECT_EQ(texts::kChecking, "Checking for updates\u2026");
    EXPECT_EQ(texts::kUpToDate, "You have the latest version.");
    EXPECT_EQ(texts::kOffInThisRun, "Updates are off in this run.");
}

TEST(UpdateRules, The_version_line_names_the_last_successful_check_in_local_time)
{
    const wizz::Locale& en = *wizz::Locale::en_us();
    const Version v{2, 1, 0};
    const wizz::FixedClock clock = settings_clock();
    EXPECT_EQ(texts::version_line(v, std::nullopt, clock, en), "WizzairHelper 2.1.0 \u00B7 never checked");
    EXPECT_EQ(texts::version_line(v, clock.utc_now(), clock, en), "WizzairHelper 2.1.0 \u00B7 last checked today at 15:53");
    EXPECT_EQ(texts::version_line(v, wizz::make_utc(wizz::Date::from_ymd(2026, 11, 17), 0, 0), clock, en),
              "WizzairHelper 2.1.0 \u00B7 last checked today at 00:00");
    EXPECT_EQ(texts::version_line(v, wizz::make_utc(wizz::Date::from_ymd(2026, 11, 16), 9, 5), clock, en),
              "WizzairHelper 2.1.0 \u00B7 last checked yesterday at 09:05");
    EXPECT_EQ(texts::version_line(v, wizz::make_utc(wizz::Date::from_ymd(2026, 11, 2), 21, 40), clock, en),
              "WizzairHelper 2.1.0 \u00B7 last checked on Mon 02 Nov 2026 at 21:40");

    // In the local zone: 22:30 UTC on the 16th is 00:30 on the 17th at UTC+2.
    const wizz::FixedClock plus_two = settings_clock(2h);
    EXPECT_EQ(texts::version_line(v, wizz::make_utc(wizz::Date::from_ymd(2026, 11, 16), 22, 30), plus_two, en),
              "WizzairHelper 2.1.0 \u00B7 last checked today at 00:30");
}

TEST(UpdateRules, After_a_failed_check_the_version_line_names_the_last_successful_check)
{
    // The owner's wording (2026-11-17): above the red failure the time is not the last
    // check's, so the line says whose it is; normally it still says "last checked".
    using texts::LastCheck;
    const wizz::Locale& en = *wizz::Locale::en_us();
    const Version v{2, 1, 0};
    const wizz::FixedClock clock = settings_clock();
    const wizz::UtcInstant ten = wizz::make_utc(wizz::Date::from_ymd(2026, 11, 17), 10, 0);
    EXPECT_EQ(texts::version_line(v, ten, clock, en, LastCheck::failed),
              "WizzairHelper 2.1.0 \u00B7 last successful check today at 10:00");
    EXPECT_EQ(texts::version_line(v, wizz::make_utc(wizz::Date::from_ymd(2026, 11, 16), 9, 5), clock, en,
                                  LastCheck::failed),
              "WizzairHelper 2.1.0 \u00B7 last successful check yesterday at 09:05");
    EXPECT_EQ(texts::version_line(v, wizz::make_utc(wizz::Date::from_ymd(2026, 11, 2), 21, 40), clock, en,
                                  LastCheck::failed),
              "WizzairHelper 2.1.0 \u00B7 last successful check on Mon 02 Nov 2026 at 21:40");
    EXPECT_EQ(texts::version_line(v, std::nullopt, clock, en, LastCheck::failed),
              "WizzairHelper 2.1.0 \u00B7 no successful check yet")
        << "tried, never worked: not \"never checked\"";

    EXPECT_EQ(texts::version_line(v, ten, clock, en, LastCheck::succeeded),
              "WizzairHelper 2.1.0 \u00B7 last checked today at 10:00");
    EXPECT_EQ(texts::version_line(v, std::nullopt, clock, en, LastCheck::succeeded),
              "WizzairHelper 2.1.0 \u00B7 never checked");
    EXPECT_EQ(texts::version_line(v, ten, clock, en), texts::version_line(v, ten, clock, en, LastCheck::succeeded))
        << "the usual wording by default";
}

TEST(UpdateRules, Failures_in_plain_words)
{
    EXPECT_EQ(texts::check_failed_status(error(UpdateErrorKind::network, "Update check failed: network error: x")),
              "Could not reach GitHub. Check your internet connection and try again.");
    EXPECT_EQ(texts::check_failed_status(error(UpdateErrorKind::http_status, "…", 503)),
              "GitHub could not answer the update check (HTTP 503). Try again later.");
    EXPECT_EQ(texts::check_failed_status(error(UpdateErrorKind::http_status, "…", 403)),
              "GitHub is limiting requests right now (HTTP 403). Try again later.");
    EXPECT_EQ(texts::check_failed_status(error(UpdateErrorKind::http_status, "…", 429)),
              "GitHub is limiting requests right now (HTTP 429). Try again later.");
    EXPECT_EQ(texts::check_failed_status(error(UpdateErrorKind::http_status, "…")),
              "GitHub could not answer the update check. Try again later.");
    EXPECT_EQ(texts::check_failed_status(error(UpdateErrorKind::bad_release, "The latest release could not be read: …", 200)),
              "The latest release on GitHub could not be read. Try again later.");

    EXPECT_EQ(texts::download_failed_status(kNew, error(UpdateErrorKind::network, "…")),
              "WizzairHelper 1.2.0 could not be downloaded. Check your internet connection and try again.");
    EXPECT_EQ(texts::download_failed_status(kNew, error(UpdateErrorKind::http_status, "…", 503)),
              "WizzairHelper 1.2.0 could not be downloaded (HTTP 503). Try again later.");
    EXPECT_EQ(texts::download_failed_status(kNew, error(UpdateErrorKind::checksum_mismatch, "…")),
              "The download of WizzairHelper 1.2.0 did not match its checksum and was deleted. Try again later.");
    EXPECT_EQ(texts::download_failed_status(kNew, error(UpdateErrorKind::missing_asset, "…")),
              "The WizzairHelper 1.2.0 release on GitHub is incomplete. Try again later.");
    EXPECT_EQ(texts::download_failed_status(kNew, error(UpdateErrorKind::bad_checksums, "…")),
              "The WizzairHelper 1.2.0 release on GitHub is incomplete. Try again later.");
    EXPECT_EQ(texts::download_failed_status(kNew, error(UpdateErrorKind::file_system, "Could not save …")),
              "WizzairHelper 1.2.0 could not be saved on this computer.");
}

TEST(UpdateRules, A_run_without_updates_says_so_and_disables_everything)
{
    EXPECT_EQ(wizz::ui::updates_off_view(Version{2, 1, 0}),
              (UpdateStatusView{.kind = UpdateStatusKind::off,
                                .version_line = "WizzairHelper 2.1.0",
                                .status = "Updates are off in this run.",
                                .checking = false,
                                .check_now_enabled = false,
                                .options_enabled = false,
                                .when_found_enabled = false,
                                .when_found_note = ""}));
}

// =====================================================================================
// Install on close (the default; 2.0 / 2.1 behaviour)
// =====================================================================================

TEST(UpdateModel, Starts_with_nothing_to_offer_and_the_default_options)
{
    const UpdateModel model{kRunning, InstallKind::installed};
    EXPECT_EQ(model.options(), UpdateOptions{});
    EXPECT_EQ(model.bar(false), UpdateBarView{});
    EXPECT_FALSE(model.bar(false).visible);
    EXPECT_FALSE(model.exit_command().has_value());
    EXPECT_FALSE(model.pending_download().has_value());
    EXPECT_FALSE(model.download_retry_due());
    EXPECT_EQ(model.running(), kRunning);
    EXPECT_EQ(model.install_kind(), InstallKind::installed);
}

TEST(UpdateModel, Up_to_date_or_no_release_is_silent)
{
    UpdateModel model{kRunning, InstallKind::installed};
    UpdateModel::CheckOutcome outcome = model.on_check(found(Version{1, 0, 0}, false));
    EXPECT_TRUE(outcome.log.empty());
    EXPECT_FALSE(outcome.download);
    outcome = model.on_check(check_error(UpdateErrorKind::no_release, "No release is published yet (GitHub answered HTTP 404)."));
    EXPECT_TRUE(outcome.log.empty());
    EXPECT_FALSE(outcome.download);
    outcome = model.on_check(check_error(UpdateErrorKind::no_release, "The latest release is a prerelease."));
    EXPECT_TRUE(outcome.log.empty());
    EXPECT_FALSE(model.bar(false).visible);
    EXPECT_FALSE(model.exit_command().has_value());
}

TEST(UpdateModel, A_failed_check_is_one_log_line_and_nothing_else)
{
    UpdateModel model{kRunning, InstallKind::installed};
    for (const UpdateErrorKind kind : {UpdateErrorKind::network, UpdateErrorKind::http_status, UpdateErrorKind::bad_release}) {
        const UpdateModel::CheckOutcome outcome = model.on_check(check_error(kind, "Update check failed: GitHub answered HTTP 500."));
        EXPECT_EQ(outcome.log, lines({"Update check failed: GitHub answered HTTP 500."}));
        EXPECT_FALSE(outcome.download);
    }
    EXPECT_FALSE(model.bar(false).visible);
}

TEST(UpdateModel, An_installed_copy_downloads_then_offers_restart)
{
    UpdateModel model{kRunning, InstallKind::installed};
    const UpdateModel::CheckOutcome outcome = model.on_check(found(kNew));
    EXPECT_EQ(outcome.log, lines({"Update available: WizzairHelper 1.2.0 (running 1.0.0)."}));
    EXPECT_TRUE(outcome.download);
    ASSERT_TRUE(model.pending_download().has_value());
    EXPECT_EQ(model.pending_download()->version, kNew);
    model.begin_download();
    EXPECT_FALSE(model.bar(false).visible) << "nothing is offered before the installer is verified";
    EXPECT_FALSE(model.exit_command().has_value());

    const UpdateModel::DownloadOutcome done = model.on_download(downloaded(kNew), false);
    EXPECT_EQ(done.log, lines({"Update 1.2.0 downloaded and verified."}));
    EXPECT_FALSE(done.restart) << "install on close waits for the user to close the app";
    EXPECT_FALSE(model.pending_download().has_value());
    ASSERT_TRUE(model.ready().has_value());
    EXPECT_EQ(*model.ready(), (wizz::ui::ReadyInstaller{.version = kNew, .path = installer_path(kNew)}));
    EXPECT_EQ(model.bar(false),
              (UpdateBarView{.visible = true,
                             .text = "WizzairHelper 1.2.0 is ready to install. It will be installed when you close "
                                     "WizzairHelper.",
                             .action = "Restart and update",
                             .action_enabled = true}));
    EXPECT_FALSE(model.bar(true).action_enabled) << "Restart and update waits while a search is working";
    EXPECT_TRUE(model.bar(true).visible);
}

TEST(UpdateModel, The_exit_command_is_the_contract_silent_install)
{
    UpdateModel model{kRunning, InstallKind::installed};
    (void)model.on_check(found(kNew));
    (void)run_download(model, downloaded(kNew));

    std::optional<update::InstallerCommand> command = model.exit_command();
    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->program, installer_path(kNew));
    EXPECT_EQ(command->arguments, relaunch_args(false));
    EXPECT_EQ(command->command_line(),
              L"\"C:\\Users\\Anna Smith\\AppData\\Local\\WizzairHelper\\updates\\downloads\\WizzairHelper-1.2.0-win64-"
              L"setup.exe\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS");

    EXPECT_EQ(model.on_action(true), BarAction::none) << "disabled while working";
    EXPECT_FALSE(model.restart_requested());
    EXPECT_EQ(model.on_action(false), BarAction::restart);
    EXPECT_TRUE(model.restart_requested());
    command = model.exit_command();
    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->arguments, relaunch_args(true));
}

TEST(UpdateModel, Restart_needs_a_ready_installer)
{
    UpdateModel model{kRunning, InstallKind::installed};
    EXPECT_EQ(model.on_action(false), BarAction::none);
    (void)model.on_check(found(kNew));
    model.begin_download();
    EXPECT_EQ(model.on_action(false), BarAction::none) << "still downloading";
    EXPECT_FALSE(model.restart_requested());
    EXPECT_FALSE(model.exit_command().has_value());
}

TEST(UpdateModel, Later_hides_the_bar_but_the_installer_still_runs_on_exit)
{
    UpdateModel model{kRunning, InstallKind::installed};
    (void)model.on_check(found(kNew));
    (void)run_download(model, downloaded(kNew));
    model.dismiss();
    EXPECT_TRUE(model.dismissed());
    EXPECT_FALSE(model.bar(false).visible);
    ASSERT_TRUE(model.exit_command().has_value());
    EXPECT_EQ(model.exit_command()->arguments, relaunch_args(false));

    // A newer release later in the session replaces the installer; the bar stays hidden.
    UpdateModel::CheckOutcome outcome = model.on_check(found(Version{1, 3, 0}));
    EXPECT_EQ(outcome.log, lines({"Update available: WizzairHelper 1.3.0 (running 1.0.0)."}));
    EXPECT_TRUE(outcome.download);
    (void)run_download(model, downloaded(Version{1, 3, 0}));
    EXPECT_FALSE(model.bar(false).visible);
    EXPECT_EQ(model.exit_command()->program, installer_path(Version{1, 3, 0}));
}

TEST(UpdateModel, A_portable_copy_offers_the_release_page)
{
    UpdateModel model{kRunning, InstallKind::portable};
    const UpdateModel::CheckOutcome outcome = model.on_check(found(kNew));
    EXPECT_EQ(outcome.log, lines({"Update available: WizzairHelper 1.2.0 (running 1.0.0)."}));
    EXPECT_FALSE(outcome.download) << "a portable folder is never replaced";
    EXPECT_FALSE(model.pending_download().has_value());
    ASSERT_TRUE(model.available().has_value());
    EXPECT_EQ(model.available()->page_url, "https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v1.2.0");
    const UpdateBarView expected{.visible = true,
                                 .text = "WizzairHelper 1.2.0 is available.",
                                 .action = "Download",
                                 .action_enabled = true};
    EXPECT_EQ(model.bar(false), expected);
    EXPECT_EQ(model.bar(true), expected) << "opening a web page does not wait for a search";
    EXPECT_FALSE(model.exit_command().has_value());
    EXPECT_EQ(model.on_action(true), BarAction::open_release_page);
    EXPECT_FALSE(model.restart_requested());

    model.dismiss();
    EXPECT_FALSE(model.bar(false).visible);
}

TEST(UpdateModel, The_same_release_is_announced_once_per_session)
{
    UpdateModel portable{kRunning, InstallKind::portable};
    (void)portable.on_check(found(kNew));
    UpdateModel::CheckOutcome again = portable.on_check(found(kNew));
    EXPECT_TRUE(again.log.empty());
    EXPECT_FALSE(again.download);
    again = portable.on_check(found(Version{1, 3, 0}));
    EXPECT_EQ(again.log, lines({"Update available: WizzairHelper 1.3.0 (running 1.0.0)."}));
    EXPECT_EQ(portable.available()->version, (Version{1, 3, 0}));
    EXPECT_EQ(portable.bar(false).text, "WizzairHelper 1.3.0 is available.");

    UpdateModel installed{kRunning, InstallKind::installed};
    (void)installed.on_check(found(kNew));
    (void)run_download(installed, downloaded(kNew));
    again = installed.on_check(found(kNew));
    EXPECT_TRUE(again.log.empty());
    EXPECT_FALSE(again.download) << "the verified installer is kept";
}

TEST(UpdateModel, A_failed_download_is_logged_and_retried)
{
    UpdateModel model{kRunning, InstallKind::installed};
    (void)model.on_check(found(kNew));
    EXPECT_EQ(run_download(model, download_error(UpdateErrorKind::http_status,
                                                 "Could not download WizzairHelper-1.2.0-win64-setup.exe after 5 tries: HTTP 503."))
                  .log,
              lines({"Update download failed: Could not download WizzairHelper-1.2.0-win64-setup.exe after 5 tries: HTTP 503."}));
    EXPECT_FALSE(model.bar(false).visible);
    EXPECT_FALSE(model.exit_command().has_value());
    EXPECT_TRUE(model.download_retry_due()) << "an HTTP status may heal: the next re-evaluation downloads again";
    ASSERT_TRUE(model.pending_download().has_value());

    (void)run_download(model, download_error(UpdateErrorKind::network, "Could not download …: timeout: x"));
    EXPECT_TRUE(model.download_retry_due());

    (void)run_download(model, download_error(UpdateErrorKind::checksum_mismatch, "… does not match SHA256SUMS.txt …"));
    EXPECT_FALSE(model.download_retry_due()) << "a broken release waits for the next scheduled check";
    const UpdateModel::CheckOutcome again = model.on_check(found(kNew));
    EXPECT_TRUE(again.log.empty()) << "already announced";
    EXPECT_TRUE(again.download) << "the next check tries the same release again";

    (void)run_download(model, downloaded(kNew));
    EXPECT_FALSE(model.download_retry_due());
    EXPECT_TRUE(model.bar(false).visible);
}

TEST(UpdateModel, An_older_release_never_replaces_a_newer_one)
{
    UpdateModel model{kRunning, InstallKind::installed};
    (void)model.on_check(found(Version{1, 3, 0}));
    (void)run_download(model, downloaded(Version{1, 3, 0}));
    const UpdateModel::CheckOutcome outcome = model.on_check(found(Version{1, 2, 5}));
    EXPECT_TRUE(outcome.log.empty());
    EXPECT_FALSE(outcome.download);
    EXPECT_EQ(model.ready()->version, (Version{1, 3, 0}));
}

TEST(UpdateModel, A_check_during_a_download_of_the_same_release_starts_nothing)
{
    UpdateModel model{kRunning, InstallKind::installed};
    (void)model.on_check(found(kNew));
    model.begin_download();
    const UpdateModel::CheckOutcome outcome = model.on_check(found(kNew), CheckOrigin::manual);
    EXPECT_TRUE(outcome.log.empty());
    EXPECT_FALSE(outcome.download) << "one download at a time";
}

// =====================================================================================
// The three choices, side by side (D-61)
// =====================================================================================

struct Expected {
    bool downloads_on_its_own;   ///< the check asks for the download
    UpdateBarView bar_after_check;
    std::string ready_text;      ///< the bar once the installer is verified ("" = no verified installer)
    bool installs_on_close;      ///< closing the app installs the verified installer
};

void expect_choice(WhenFound when_found, const Expected& expected)
{
    SCOPED_TRACE(std::string{texts::when_found_label(when_found)});
    UpdateModel model{kRunning, InstallKind::installed, with(when_found)};
    const UpdateModel::CheckOutcome outcome = model.on_check(found(kNew));
    EXPECT_EQ(outcome.log, lines({"Update available: WizzairHelper 1.2.0 (running 1.0.0)."}));
    EXPECT_EQ(outcome.download, expected.downloads_on_its_own);
    EXPECT_EQ(model.bar(false), expected.bar_after_check);
    EXPECT_FALSE(model.exit_command().has_value()) << "nothing to install yet";

    if (expected.downloads_on_its_own) {
        const UpdateModel::DownloadOutcome done = run_download(model, downloaded(kNew));
        EXPECT_FALSE(done.restart);
    } else {
        ASSERT_EQ(model.on_action(false), BarAction::download);
        EXPECT_EQ(model.bar(false), (UpdateBarView{.visible = true,
                                                   .text = "Downloading WizzairHelper 1.2.0\u2026",
                                                   .action = "",
                                                   .action_enabled = false}));
        const UpdateModel::DownloadOutcome done = run_download(model, downloaded(kNew));
        EXPECT_TRUE(done.restart) << "Update now ends in a restart";
        EXPECT_TRUE(model.restart_requested());
        ASSERT_TRUE(model.exit_command().has_value());
        EXPECT_EQ(model.exit_command()->arguments, relaunch_args(true));
        return;
    }
    EXPECT_EQ(model.bar(false), (UpdateBarView{.visible = true,
                                               .text = expected.ready_text,
                                               .action = "Restart and update",
                                               .action_enabled = true}));
    EXPECT_EQ(model.exit_command().has_value(), expected.installs_on_close);
    if (expected.installs_on_close) {
        EXPECT_EQ(model.exit_command()->arguments, relaunch_args(false));
    }
    EXPECT_EQ(model.on_action(false), BarAction::restart);
    ASSERT_TRUE(model.exit_command().has_value()) << "Restart and update installs in every choice";
    EXPECT_EQ(model.exit_command()->arguments, relaunch_args(true));
}

TEST(UpdateModel, Install_on_close_downloads_offers_restart_and_installs_when_the_app_closes)
{
    expect_choice(WhenFound::install_on_close,
                  {.downloads_on_its_own = true,
                   .bar_after_check = {},
                   .ready_text = "WizzairHelper 1.2.0 is ready to install. It will be installed when you close WizzairHelper.",
                   .installs_on_close = true});
}

TEST(UpdateModel, Download_then_ask_downloads_offers_restart_and_leaves_it_on_close)
{
    expect_choice(WhenFound::download_then_ask, {.downloads_on_its_own = true,
                                                 .bar_after_check = {},
                                                 .ready_text = "WizzairHelper 1.2.0 is ready to install.",
                                                 .installs_on_close = false});
}

TEST(UpdateModel, Notify_only_tells_and_Update_now_downloads_then_restarts)
{
    expect_choice(WhenFound::notify_only, {.downloads_on_its_own = false,
                                           .bar_after_check = {.visible = true,
                                                               .text = "WizzairHelper 1.2.0 is available.",
                                                               .action = "Update now",
                                                               .action_enabled = true},
                                           .ready_text = "",
                                           .installs_on_close = false});
}

TEST(UpdateModel, A_portable_copy_offers_the_release_page_whatever_the_choice)
{
    for (const WhenFound when_found : {WhenFound::install_on_close, WhenFound::download_then_ask, WhenFound::notify_only}) {
        SCOPED_TRACE(std::string{texts::when_found_label(when_found)});
        UpdateModel model{kRunning, InstallKind::portable, with(when_found)};
        EXPECT_FALSE(model.on_check(found(kNew)).download);
        EXPECT_EQ(model.bar(false).action, "Download");
        EXPECT_EQ(model.on_action(false), BarAction::open_release_page);
        EXPECT_FALSE(model.exit_command().has_value());
        for (const WhenFound other : {WhenFound::install_on_close, WhenFound::download_then_ask, WhenFound::notify_only}) {
            EXPECT_FALSE(model.set_options(with(other))) << "a portable copy never downloads";
        }
    }
}

TEST(UpdateModel, Download_then_ask_restart_and_update_is_disabled_while_working)
{
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::download_then_ask)};
    (void)model.on_check(found(kNew));
    (void)run_download(model, downloaded(kNew));
    EXPECT_FALSE(model.bar(true).action_enabled);
    EXPECT_EQ(model.on_action(true), BarAction::none);
    EXPECT_FALSE(model.exit_command().has_value());
    model.dismiss();
    EXPECT_FALSE(model.bar(false).visible);
    EXPECT_FALSE(model.exit_command().has_value()) << "Later and close: not installed";
}

TEST(UpdateModel, Update_now_waits_while_working_and_never_restarts_into_a_running_search)
{
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::notify_only)};
    (void)model.on_check(found(kNew));
    EXPECT_FALSE(model.bar(true).action_enabled) << "Update now ends in a restart: it waits like Restart and update";
    EXPECT_EQ(model.on_action(true), BarAction::none);
    EXPECT_FALSE(model.install_requested());

    ASSERT_EQ(model.on_action(false), BarAction::download);
    EXPECT_TRUE(model.install_requested());
    EXPECT_EQ(model.on_action(false), BarAction::none) << "already on its way";
    // A search started while the installer downloaded: no restart into it.
    const UpdateModel::DownloadOutcome done = run_download(model, downloaded(kNew), true);
    EXPECT_FALSE(done.restart);
    EXPECT_FALSE(model.restart_requested());
    EXPECT_FALSE(model.install_requested());
    EXPECT_EQ(model.bar(true), (UpdateBarView{.visible = true,
                                              .text = "WizzairHelper 1.2.0 is ready to install.",
                                              .action = "Restart and update",
                                              .action_enabled = false}));
    EXPECT_FALSE(model.exit_command().has_value()) << "notify only never installs on close";
    EXPECT_EQ(model.on_action(false), BarAction::restart);
    EXPECT_EQ(model.exit_command()->arguments, relaunch_args(true));
}

TEST(UpdateModel, Update_now_offers_itself_again_after_a_failed_download)
{
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::notify_only)};
    (void)model.on_check(found(kNew));
    ASSERT_EQ(model.on_action(false), BarAction::download);
    const UpdateModel::DownloadOutcome done =
        run_download(model, download_error(UpdateErrorKind::http_status, "Could not download … after 5 tries: HTTP 503.", 503));
    EXPECT_EQ(done.log, lines({"Update download failed: Could not download … after 5 tries: HTTP 503."}));
    EXPECT_FALSE(done.restart);
    EXPECT_FALSE(model.install_requested());
    EXPECT_EQ(model.bar(false).action, "Update now");
    EXPECT_FALSE(model.download_retry_due()) << "notify only never downloads on its own";
    EXPECT_EQ(status(model).kind, UpdateStatusKind::failed);
    EXPECT_EQ(status(model).status, "WizzairHelper 1.2.0 could not be downloaded (HTTP 503). Try again later.");
    EXPECT_EQ(model.on_action(false), BarAction::download) << "the user may try again";
}

TEST(UpdateModel, Later_during_Update_now_cancels_the_restart)
{
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::notify_only)};
    (void)model.on_check(found(kNew));
    ASSERT_EQ(model.on_action(false), BarAction::download);
    model.begin_download();
    model.dismiss();
    EXPECT_FALSE(model.bar(false).visible);
    const UpdateModel::DownloadOutcome done = model.on_download(downloaded(kNew), false);
    EXPECT_FALSE(done.restart) << "Later: not now";
    EXPECT_FALSE(model.exit_command().has_value());
    ASSERT_TRUE(model.ready().has_value()) << "the verified installer is kept for the next time";
}

TEST(UpdateModel, Notify_only_offers_a_newer_release_over_an_older_verified_one)
{
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::notify_only)};
    (void)model.on_check(found(kNew));
    ASSERT_EQ(model.on_action(false), BarAction::download);
    (void)run_download(model, downloaded(kNew), true);  // ready, no restart (working)
    (void)model.on_check(found(Version{1, 3, 0}));
    EXPECT_EQ(model.bar(false).text, "WizzairHelper 1.3.0 is available.");
    EXPECT_EQ(model.bar(false).action, "Update now");
    EXPECT_EQ(status(model).status, "WizzairHelper 1.3.0 is available.");
    EXPECT_EQ(model.on_action(false), BarAction::download) << "the newest one, not the older verified installer";
}

// =====================================================================================
// Changing the options while an update is under way (D-61)
// =====================================================================================

TEST(UpdateModel, Switching_to_install_on_close_with_a_verified_download_installs_it_on_close)
{
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::download_then_ask)};
    (void)model.on_check(found(kNew));
    (void)run_download(model, downloaded(kNew));
    EXPECT_FALSE(model.exit_command().has_value());
    EXPECT_FALSE(model.set_options(with(WhenFound::install_on_close))) << "nothing to download";
    ASSERT_TRUE(model.exit_command().has_value());
    EXPECT_EQ(model.exit_command()->arguments, relaunch_args(false));
    EXPECT_EQ(model.bar(false).text,
              "WizzairHelper 1.2.0 is ready to install. It will be installed when you close WizzairHelper.");

    EXPECT_FALSE(model.set_options(with(WhenFound::download_then_ask)));
    EXPECT_FALSE(model.exit_command().has_value()) << "and back: closing leaves it";
    EXPECT_EQ(model.bar(false).text, "WizzairHelper 1.2.0 is ready to install.");

    EXPECT_FALSE(model.set_options(with(WhenFound::notify_only)));
    EXPECT_FALSE(model.exit_command().has_value());
    EXPECT_EQ(model.bar(false), (UpdateBarView{.visible = true,
                                               .text = "WizzairHelper 1.2.0 is ready to install.",
                                               .action = "Restart and update",
                                               .action_enabled = true}))
        << "already downloaded: it is offered as ready";
}

TEST(UpdateModel, Switching_from_notify_only_to_a_downloading_choice_downloads_at_once)
{
    for (const WhenFound target : {WhenFound::install_on_close, WhenFound::download_then_ask}) {
        SCOPED_TRACE(std::string{texts::when_found_label(target)});
        UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::notify_only)};
        (void)model.on_check(found(kNew));
        EXPECT_TRUE(model.set_options(with(target)));
        EXPECT_FALSE(model.bar(false).visible) << "the downloading choices offer only a verified installer";
        model.begin_download();
        EXPECT_FALSE(model.set_options(with(target))) << "already downloading";
        (void)model.on_download(downloaded(kNew), false);
        EXPECT_EQ(model.exit_command().has_value(), target == WhenFound::install_on_close);
    }
}

TEST(UpdateModel, Switching_with_automatic_checks_off_downloads_nothing_and_keeps_Update_now)
{
    // Check now found 1.2.0 while the choice only tells; the user then picks a choice that
    // downloads. With automatic checks off nothing is sent on its own, so the offer keeps
    // its action: losing it would leave the update with no way to install it.
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::notify_only, false)};
    (void)model.on_check(found(kNew), CheckOrigin::manual);
    for (const WhenFound target : {WhenFound::install_on_close, WhenFound::download_then_ask}) {
        SCOPED_TRACE(std::string{texts::when_found_label(target)});
        EXPECT_FALSE(model.set_options(with(target, false)))
            << "no request to GitHub unless the user presses Check now or Update now";
        EXPECT_FALSE(model.download_wanted());
        EXPECT_EQ(model.bar(false), (UpdateBarView{.visible = true,
                                                   .text = "WizzairHelper 1.2.0 is available.",
                                                   .action = "Update now",
                                                   .action_enabled = true}));
        EXPECT_EQ(status(model).status, "WizzairHelper 1.2.0 is available.");
        EXPECT_FALSE(model.exit_command().has_value()) << "nothing was downloaded, nothing installs";
    }
    ASSERT_EQ(model.on_action(false), BarAction::download);
    EXPECT_TRUE(model.download_wanted());
    EXPECT_TRUE(run_download(model, downloaded(kNew)).restart) << "Update now: verified, then restart and install";

    // Check now finding it again downloads it with a choice that downloads.
    UpdateModel again{kRunning, InstallKind::installed, with(WhenFound::notify_only, false)};
    (void)again.on_check(found(kNew), CheckOrigin::manual);
    (void)again.set_options(with(WhenFound::install_on_close, false));
    EXPECT_TRUE(again.on_check(found(kNew), CheckOrigin::manual).download);
}

TEST(UpdateModel, With_automatic_checks_off_a_failed_download_offers_Update_now)
{
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::install_on_close, false)};
    EXPECT_TRUE(model.on_check(found(kNew), CheckOrigin::manual).download) << "Check now asked: it downloads";
    EXPECT_FALSE(model.bar(false).visible) << "its download is on its way: no Update now in between";
    model.begin_download();
    EXPECT_FALSE(model.bar(false).visible) << "install on close shows the bar once verified";
    (void)model.on_download(download_error(UpdateErrorKind::network, "… timeout …"), false);
    EXPECT_FALSE(model.download_retry_due()) << "nothing retries it while checks are off";
    EXPECT_EQ(model.bar(false).action, "Update now") << "so the user can";
    EXPECT_EQ(status(model).kind, UpdateStatusKind::failed);

    (void)model.set_options(with(WhenFound::install_on_close, true));
    EXPECT_FALSE(model.bar(false).visible) << "checks on: the hourly retry downloads it";
    EXPECT_TRUE(model.download_retry_due());
}

TEST(UpdateModel, A_download_not_started_yet_is_withdrawn_when_the_options_stop_downloading)
{
    // The controller queues a download behind running work and asks download_wanted()
    // before it starts it.
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::notify_only)};
    (void)model.on_check(found(kNew));
    EXPECT_FALSE(model.download_wanted()) << "notify only waits for Update now";
    ASSERT_TRUE(model.set_options(with(WhenFound::install_on_close)));
    EXPECT_TRUE(model.download_wanted());
    EXPECT_FALSE(model.set_options(with(WhenFound::notify_only)));
    EXPECT_FALSE(model.download_wanted()) << "back to only telling: withdrawn";
    EXPECT_EQ(model.bar(false).action, "Update now");
    ASSERT_TRUE(model.set_options(with(WhenFound::download_then_ask)));
    EXPECT_TRUE(model.download_wanted());
    EXPECT_FALSE(model.set_options(with(WhenFound::download_then_ask, false)));
    EXPECT_FALSE(model.download_wanted()) << "automatic checks off: withdrawn too";
    EXPECT_FALSE(model.set_options(with(WhenFound::download_then_ask, false, CheckFrequency::weekly)));
    EXPECT_FALSE(model.download_wanted());

    ASSERT_EQ(model.on_action(false), BarAction::download) << "Update now";
    EXPECT_TRUE(model.download_wanted());
    model.dismiss();
    EXPECT_FALSE(model.download_wanted()) << "Later before it started: dropped";
    ASSERT_EQ(model.set_options(with(WhenFound::install_on_close)), true);
    model.begin_download();
    EXPECT_FALSE(model.download_wanted()) << "running: nothing more to start";
    EXPECT_FALSE(model.set_options(with(WhenFound::notify_only)));
    (void)model.on_download(downloaded(kNew), false);
    EXPECT_TRUE(model.ready().has_value()) << "a running download keeps running";
}

TEST(UpdateModel, Switching_does_not_repeat_a_download_that_already_failed)
{
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::download_then_ask)};
    (void)model.on_check(found(kNew));
    (void)run_download(model, download_error(UpdateErrorKind::checksum_mismatch, "… does not match …"));
    EXPECT_FALSE(model.set_options(with(WhenFound::install_on_close))) << "a broken release waits for the next check";
}

TEST(UpdateModel, Switching_away_while_downloading_keeps_the_download_but_not_the_install_on_close)
{
    UpdateModel model{kRunning, InstallKind::installed};
    (void)model.on_check(found(kNew));
    model.begin_download();
    EXPECT_FALSE(model.set_options(with(WhenFound::notify_only)));
    EXPECT_EQ(model.bar(false).action, "Update now") << "not verified yet: offered like any notify-only update";
    EXPECT_EQ(model.on_action(false), BarAction::none) << "the running download restarts when it is verified";
    EXPECT_TRUE(model.install_requested());
    const UpdateModel::DownloadOutcome done = model.on_download(downloaded(kNew), false);
    EXPECT_TRUE(done.restart);
    EXPECT_EQ(model.exit_command()->arguments, relaunch_args(true));
}

TEST(UpdateModel, Switching_to_install_on_close_after_Update_now_still_restarts)
{
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::notify_only)};
    (void)model.on_check(found(kNew));
    ASSERT_EQ(model.on_action(false), BarAction::download);
    model.begin_download();
    EXPECT_FALSE(model.set_options(with(WhenFound::install_on_close))) << "already downloading";
    EXPECT_TRUE(model.on_download(downloaded(kNew), false).restart);
}

TEST(UpdateModel, Download_retries_follow_the_options)
{
    UpdateModel model{kRunning, InstallKind::installed};
    (void)model.on_check(found(kNew));
    (void)run_download(model, download_error(UpdateErrorKind::network, "… timeout …"));
    EXPECT_TRUE(model.download_retry_due());
    (void)model.set_options(with(WhenFound::install_on_close, false));
    EXPECT_FALSE(model.download_retry_due()) << "automatic checks off: no request to GitHub";
    (void)model.set_options(with(WhenFound::notify_only));
    EXPECT_FALSE(model.download_retry_due()) << "notify only never downloads on its own";
    EXPECT_EQ(model.bar(false).action, "Update now");
    (void)model.set_options(with(WhenFound::download_then_ask));
    EXPECT_TRUE(model.download_retry_due());
    model.begin_download();
    EXPECT_FALSE(model.download_retry_due()) << "not while it runs";
}

// =====================================================================================
// Checks: the policy, manual checks and the Settings window's status
// =====================================================================================

TEST(UpdateModel, The_check_policy_follows_the_options)
{
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::install_on_close, true, CheckFrequency::weekly)};
    EXPECT_EQ(model.check_policy(), (update::CheckPolicy{.automatic = true, .frequency = CheckFrequency::weekly}));
    (void)model.on_check(found(kNew));
    EXPECT_EQ(model.check_policy(), (update::CheckPolicy{.automatic = true, .frequency = CheckFrequency::weekly}))
        << "the period alone, also with an update found (D-59)";
    (void)model.set_options(with(WhenFound::notify_only, false, CheckFrequency::monthly));
    EXPECT_EQ(model.check_policy(), (update::CheckPolicy{.automatic = false, .frequency = CheckFrequency::monthly}));
}

TEST(UpdateModel, A_manual_check_reports_checking_then_its_result)
{
    UpdateModel model{kRunning, InstallKind::installed};
    model.begin_manual_check();
    EXPECT_TRUE(model.checking());
    UpdateStatusView view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::checking);
    EXPECT_EQ(view.status, "Checking for updates\u2026");
    EXPECT_TRUE(view.checking);
    EXPECT_FALSE(view.check_now_enabled);

    // An automatic check finishing meanwhile does not end the manual one.
    (void)model.on_check(found(Version{1, 0, 0}, false), CheckOrigin::automatic);
    EXPECT_TRUE(model.checking());

    (void)model.on_check(found(Version{1, 0, 0}, false), CheckOrigin::manual);
    EXPECT_FALSE(model.checking());
    view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::up_to_date);
    EXPECT_EQ(view.status, "You have the latest version.");
    EXPECT_TRUE(view.check_now_enabled);

    model.begin_manual_check();
    const UpdateModel::CheckOutcome failed =
        model.on_check(check_error(UpdateErrorKind::network, "Update check failed: network error: no route"),
                       CheckOrigin::manual);
    EXPECT_EQ(failed.log, lines({"Update check failed: network error: no route"}));
    view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::failed);
    EXPECT_EQ(view.status, "Could not reach GitHub. Check your internet connection and try again.");

    model.begin_manual_check();
    const UpdateModel::CheckOutcome newer = model.on_check(found(kNew), CheckOrigin::manual);
    EXPECT_TRUE(newer.download);
    view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::available);
    EXPECT_EQ(view.status, "WizzairHelper 1.2.0 is available.");
    model.begin_download();
    view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::downloading);
    EXPECT_EQ(view.status, "Downloading WizzairHelper 1.2.0\u2026");
    EXPECT_FALSE(view.check_now_enabled) << "nothing to check while the update downloads";
    (void)model.on_download(downloaded(kNew), false);
    view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::ready);
    EXPECT_EQ(view.status, "WizzairHelper 1.2.0 is ready to install.");
    EXPECT_TRUE(view.check_now_enabled);
}

TEST(UpdateModel, A_manual_check_shows_the_bar_again_after_Later)
{
    UpdateModel model{kRunning, InstallKind::portable};
    (void)model.on_check(found(kNew));
    model.dismiss();
    (void)model.on_check(found(kNew), CheckOrigin::automatic);
    EXPECT_FALSE(model.bar(false).visible) << "an automatic check respects Later";
    (void)model.on_check(found(kNew), CheckOrigin::manual);
    EXPECT_TRUE(model.bar(false).visible) << "Check now: the user asked";

    UpdateModel up_to_date{kRunning, InstallKind::portable};
    up_to_date.dismiss();
    (void)up_to_date.on_check(found(kRunning, false), CheckOrigin::manual);
    EXPECT_TRUE(up_to_date.dismissed()) << "nothing to show";
}

TEST(UpdateModel, The_status_before_any_check_comes_from_update_state_json)
{
    UpdateModel model{Version{2, 1, 0}, InstallKind::installed};
    UpdateStatusView view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::not_checked);
    EXPECT_EQ(view.version_line, "WizzairHelper 2.1.0 \u00B7 never checked");
    EXPECT_EQ(view.status, "");
    EXPECT_TRUE(view.check_now_enabled);
    EXPECT_TRUE(view.options_enabled);
    EXPECT_TRUE(view.when_found_enabled);
    EXPECT_EQ(view.when_found_note, "");

    const wizz::UtcInstant earlier = wizz::make_utc(wizz::Date::from_ymd(2026, 11, 17), 9, 12);
    model.set_saved_state(update::UpdateState{.last_success = earlier, .latest_seen = Version{2, 1, 0}, .last_failure = {}});
    view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::up_to_date);
    EXPECT_EQ(view.version_line, "WizzairHelper 2.1.0 \u00B7 last checked today at 09:12");

    model.set_saved_state(update::UpdateState{.last_success = earlier, .latest_seen = std::nullopt, .last_failure = {}});
    EXPECT_EQ(status(model).kind, UpdateStatusKind::up_to_date) << "no release published";

    model.set_saved_state(update::UpdateState{.last_success = earlier, .latest_seen = Version{2, 2, 0}, .last_failure = {}});
    view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::available);
    EXPECT_EQ(view.status, "WizzairHelper 2.2.0 is available.");

    // This session's check wins over the saved state.
    (void)model.on_check(check_error(UpdateErrorKind::no_release, "No release."));
    EXPECT_EQ(status(model).kind, UpdateStatusKind::up_to_date);
}

TEST(UpdateModel, The_version_line_says_last_successful_check_while_the_last_check_failed)
{
    const wizz::UtcInstant ten = wizz::make_utc(wizz::Date::from_ymd(2026, 11, 17), 10, 0);
    const wizz::UtcInstant later = wizz::make_utc(wizz::Date::from_ymd(2026, 11, 17), 15, 50);
    const update::UpdateState checked_at_ten{.last_success = ten, .latest_seen = std::nullopt, .last_failure = {}};
    UpdateModel model{kRunning, InstallKind::installed};
    model.set_saved_state(checked_at_ten);
    EXPECT_EQ(status(model).version_line, "WizzairHelper 1.0.0 \u00B7 last checked today at 10:00");

    // A failed check: the red failure under the last *successful* check.
    (void)model.on_check(check_error(UpdateErrorKind::network, "Update check failed: network error: no route"));
    model.set_saved_state(update::UpdateState{.last_success = ten, .latest_seen = std::nullopt, .last_failure = later});
    UpdateStatusView view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::failed);
    EXPECT_EQ(view.status, "Could not reach GitHub. Check your internet connection and try again.");
    EXPECT_EQ(view.version_line, "WizzairHelper 1.0.0 \u00B7 last successful check today at 10:00");

    // The next check works: the usual wording, with its time.
    model.set_saved_state(update::UpdateState{.last_success = later, .latest_seen = std::nullopt, .last_failure = {}});
    (void)model.on_check(check_error(UpdateErrorKind::no_release, "No release is published yet (GitHub answered HTTP 404)."));
    view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::up_to_date);
    EXPECT_EQ(view.version_line, "WizzairHelper 1.0.0 \u00B7 last checked today at 15:50");

    // A failed download is not a failed check: the check before it worked.
    (void)model.on_check(found(kNew));
    (void)run_download(model, download_error(UpdateErrorKind::http_status, "… HTTP 503.", 503));
    view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::failed);
    EXPECT_EQ(view.version_line, "WizzairHelper 1.0.0 \u00B7 last checked today at 15:50");

    // Check now that ends with an unexpected error failed too.
    UpdateModel crashed{kRunning, InstallKind::installed};
    crashed.set_saved_state(checked_at_ten);
    crashed.begin_manual_check();
    crashed.on_unexpected_error("bad allocation");
    EXPECT_EQ(status(crashed).version_line, "WizzairHelper 1.0.0 \u00B7 last successful check today at 10:00");
}

TEST(UpdateModel, A_first_check_that_fails_has_no_successful_check_yet){
    UpdateModel model{kRunning, InstallKind::installed};
    EXPECT_EQ(status(model).version_line, "WizzairHelper 1.0.0 \u00B7 never checked");
    (void)model.on_check(check_error(UpdateErrorKind::http_status, "Update check failed: GitHub answered HTTP 503.", 503));
    const UpdateStatusView view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::failed);
    EXPECT_EQ(view.status, "GitHub could not answer the update check (HTTP 503). Try again later.");
    EXPECT_EQ(view.version_line, "WizzairHelper 1.0.0 \u00B7 no successful check yet");
}

TEST(UpdateModel, A_portable_copy_disables_the_install_choices_with_a_reason)
{
    const UpdateModel model{kRunning, InstallKind::portable};
    const UpdateStatusView view = status(model);
    EXPECT_FALSE(view.when_found_enabled);
    EXPECT_EQ(view.when_found_note,
              "This portable copy cannot install updates itself; it links to the release page instead.");
    EXPECT_TRUE(view.options_enabled) << "the check options still apply";
    EXPECT_TRUE(view.check_now_enabled);
}

TEST(UpdateModel, An_unexpected_error_ends_whatever_was_busy)
{
    UpdateModel model{kRunning, InstallKind::installed, with(WhenFound::notify_only)};
    (void)model.on_check(found(kNew));
    ASSERT_EQ(model.on_action(false), BarAction::download);
    model.begin_download();
    model.begin_manual_check();
    model.on_unexpected_error("bad allocation");
    EXPECT_FALSE(model.checking());
    EXPECT_FALSE(model.downloading());
    EXPECT_FALSE(model.install_requested());
    const UpdateStatusView view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::failed);
    EXPECT_EQ(view.status, "Something went wrong: bad allocation");
    EXPECT_TRUE(view.check_now_enabled);
    EXPECT_EQ(model.bar(false).action, "Update now") << "offered again";
}

TEST(UpdateModel, A_failed_automatic_download_shows_in_the_status)
{
    UpdateModel model{kRunning, InstallKind::installed};
    (void)model.on_check(found(kNew));
    (void)run_download(model, download_error(UpdateErrorKind::checksum_mismatch, "… does not match …"));
    UpdateStatusView view = status(model);
    EXPECT_EQ(view.kind, UpdateStatusKind::failed);
    EXPECT_EQ(view.status, "The download of WizzairHelper 1.2.0 did not match its checksum and was deleted. Try again later.");
    model.begin_download();
    EXPECT_EQ(status(model).kind, UpdateStatusKind::downloading) << "a new try replaces the failure";
}

} // namespace
