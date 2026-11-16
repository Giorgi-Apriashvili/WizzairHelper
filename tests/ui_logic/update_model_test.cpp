// The self-updater's presentation rules (DECISIONS D-45..D-49): when the app may
// check, the bar texts and buttons, the Log tab lines, what a check or download
// result means for an installed or a portable copy, "Later", "Restart and update"
// and the installer command run on exit.

#include "wizz/ui/update_model.hpp"

#include <gtest/gtest.h>

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace update = wizz::update;
using update::InstallKind;
using update::UpdateErrorKind;
using update::Version;
using wizz::ui::UpdateBarView;
using wizz::ui::UpdateModel;
using wizz::ui::updates_allowed;
namespace texts = wizz::ui::update_strings;

const Version kRunning{1, 0, 0};
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

[[nodiscard]] std::expected<update::ReleaseInfo, update::UpdateError> found(Version version, bool newer = true)
{
    return release(version, newer);
}

[[nodiscard]] std::expected<update::ReleaseInfo, update::UpdateError> check_error(UpdateErrorKind kind,
                                                                                   std::string message)
{
    return std::unexpected(update::UpdateError{.kind = kind, .message = std::move(message), .http_status = {}, .tries = {}});
}

[[nodiscard]] std::filesystem::path installer_path(Version version)
{
    return kDownloads / ("WizzairHelper-" + version.to_string() + "-win64-setup.exe");
}

[[nodiscard]] std::expected<update::DownloadedFile, update::UpdateError> downloaded(Version version)
{
    return update::DownloadedFile{.path = installer_path(version),
                                  .sha256 = std::string(64, 'a'),
                                  .reused = false,
                                  .tries = {}};
}

[[nodiscard]] std::expected<update::DownloadedFile, update::UpdateError> download_error(UpdateErrorKind kind,
                                                                                         std::string message)
{
    return std::unexpected(update::UpdateError{.kind = kind, .message = std::move(message), .http_status = {}, .tries = {}});
}

[[nodiscard]] std::vector<std::string> lines(std::initializer_list<std::string> list)
{
    return std::vector<std::string>{list};
}

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
    EXPECT_EQ(texts::ready_text(Version{1, 2, 0}),
              "WizzairHelper 1.2.0 is ready to install. It will be installed when you close WizzairHelper.");
    EXPECT_EQ(texts::available_text(Version{1, 10, 3}), "WizzairHelper 1.10.3 is available.");

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

TEST(UpdateModel, Starts_with_nothing_to_offer)
{
    const UpdateModel model{kRunning, InstallKind::installed};
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
    const UpdateModel::CheckOutcome outcome = model.on_check(found(Version{1, 2, 0}));
    EXPECT_EQ(outcome.log, lines({"Update available: WizzairHelper 1.2.0 (running 1.0.0)."}));
    EXPECT_TRUE(outcome.download);
    ASSERT_TRUE(model.pending_download().has_value());
    EXPECT_EQ(model.pending_download()->version, (Version{1, 2, 0}));
    EXPECT_FALSE(model.bar(false).visible) << "nothing is offered before the installer is verified";
    EXPECT_FALSE(model.exit_command().has_value());

    EXPECT_EQ(model.on_download(downloaded(Version{1, 2, 0})), lines({"Update 1.2.0 downloaded and verified."}));
    EXPECT_FALSE(model.pending_download().has_value());
    ASSERT_TRUE(model.ready().has_value());
    EXPECT_EQ(*model.ready(), (wizz::ui::ReadyInstaller{.version = Version{1, 2, 0}, .path = installer_path(Version{1, 2, 0})}));
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
    (void)model.on_check(found(Version{1, 2, 0}));
    (void)model.on_download(downloaded(Version{1, 2, 0}));

    std::optional<update::InstallerCommand> command = model.exit_command();
    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->program, installer_path(Version{1, 2, 0}));
    EXPECT_EQ(command->arguments,
              (std::vector<std::string>{"/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS"}));
    EXPECT_EQ(command->command_line(),
              L"\"C:\\Users\\Anna Smith\\AppData\\Local\\WizzairHelper\\updates\\downloads\\WizzairHelper-1.2.0-win64-"
              L"setup.exe\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS");

    EXPECT_FALSE(model.request_restart(true)) << "disabled while working";
    EXPECT_FALSE(model.restart_requested());
    EXPECT_TRUE(model.request_restart(false));
    EXPECT_TRUE(model.restart_requested());
    command = model.exit_command();
    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->arguments, (std::vector<std::string>{"/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                                                            "/CLOSEAPPLICATIONS", "/RELAUNCH"}));
}

TEST(UpdateModel, Restart_needs_a_ready_installer)
{
    UpdateModel model{kRunning, InstallKind::installed};
    EXPECT_FALSE(model.request_restart(false));
    (void)model.on_check(found(Version{1, 2, 0}));
    EXPECT_FALSE(model.request_restart(false)) << "still downloading";
    EXPECT_FALSE(model.exit_command().has_value());
}

TEST(UpdateModel, Later_hides_the_bar_but_the_installer_still_runs_on_exit)
{
    UpdateModel model{kRunning, InstallKind::installed};
    (void)model.on_check(found(Version{1, 2, 0}));
    (void)model.on_download(downloaded(Version{1, 2, 0}));
    model.dismiss();
    EXPECT_TRUE(model.dismissed());
    EXPECT_FALSE(model.bar(false).visible);
    ASSERT_TRUE(model.exit_command().has_value());
    EXPECT_EQ(model.exit_command()->arguments.size(), 4u) << "no /RELAUNCH";

    // A newer release later in the session replaces the installer; the bar stays hidden.
    UpdateModel::CheckOutcome outcome = model.on_check(found(Version{1, 3, 0}));
    EXPECT_EQ(outcome.log, lines({"Update available: WizzairHelper 1.3.0 (running 1.0.0)."}));
    EXPECT_TRUE(outcome.download);
    (void)model.on_download(downloaded(Version{1, 3, 0}));
    EXPECT_FALSE(model.bar(false).visible);
    EXPECT_EQ(model.exit_command()->program, installer_path(Version{1, 3, 0}));
}

TEST(UpdateModel, A_portable_copy_offers_the_release_page)
{
    UpdateModel model{kRunning, InstallKind::portable};
    const UpdateModel::CheckOutcome outcome = model.on_check(found(Version{1, 2, 0}));
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
    EXPECT_FALSE(model.request_restart(false));

    model.dismiss();
    EXPECT_FALSE(model.bar(false).visible);
}

TEST(UpdateModel, The_same_release_is_announced_once_per_session)
{
    UpdateModel portable{kRunning, InstallKind::portable};
    (void)portable.on_check(found(Version{1, 2, 0}));
    UpdateModel::CheckOutcome again = portable.on_check(found(Version{1, 2, 0}));
    EXPECT_TRUE(again.log.empty());
    EXPECT_FALSE(again.download);
    again = portable.on_check(found(Version{1, 3, 0}));
    EXPECT_EQ(again.log, lines({"Update available: WizzairHelper 1.3.0 (running 1.0.0)."}));
    EXPECT_EQ(portable.available()->version, (Version{1, 3, 0}));
    EXPECT_EQ(portable.bar(false).text, "WizzairHelper 1.3.0 is available.");

    UpdateModel installed{kRunning, InstallKind::installed};
    (void)installed.on_check(found(Version{1, 2, 0}));
    (void)installed.on_download(downloaded(Version{1, 2, 0}));
    again = installed.on_check(found(Version{1, 2, 0}));
    EXPECT_TRUE(again.log.empty());
    EXPECT_FALSE(again.download) << "the verified installer is kept";
}

TEST(UpdateModel, A_failed_download_is_logged_and_retried)
{
    UpdateModel model{kRunning, InstallKind::installed};
    (void)model.on_check(found(Version{1, 2, 0}));
    EXPECT_EQ(model.on_download(download_error(UpdateErrorKind::http_status,
                                               "Could not download WizzairHelper-1.2.0-win64-setup.exe after 5 tries: HTTP 503.")),
              lines({"Update download failed: Could not download WizzairHelper-1.2.0-win64-setup.exe after 5 tries: HTTP 503."}));
    EXPECT_FALSE(model.bar(false).visible);
    EXPECT_FALSE(model.exit_command().has_value());
    EXPECT_TRUE(model.download_retry_due()) << "an HTTP status may heal: the next re-evaluation downloads again";
    ASSERT_TRUE(model.pending_download().has_value());

    (void)model.on_download(download_error(UpdateErrorKind::network, "Could not download …: timeout: x"));
    EXPECT_TRUE(model.download_retry_due());

    (void)model.on_download(download_error(UpdateErrorKind::checksum_mismatch, "… does not match SHA256SUMS.txt …"));
    EXPECT_FALSE(model.download_retry_due()) << "a broken release waits for the next daily check";
    const UpdateModel::CheckOutcome again = model.on_check(found(Version{1, 2, 0}));
    EXPECT_TRUE(again.log.empty()) << "already announced";
    EXPECT_TRUE(again.download) << "the next daily check tries the same release again";

    (void)model.on_download(downloaded(Version{1, 2, 0}));
    EXPECT_FALSE(model.download_retry_due());
    EXPECT_TRUE(model.bar(false).visible);
}

TEST(UpdateModel, An_older_release_never_replaces_a_newer_one)
{
    UpdateModel model{kRunning, InstallKind::installed};
    (void)model.on_check(found(Version{1, 3, 0}));
    (void)model.on_download(downloaded(Version{1, 3, 0}));
    const UpdateModel::CheckOutcome outcome = model.on_check(found(Version{1, 2, 5}));
    EXPECT_TRUE(outcome.log.empty());
    EXPECT_FALSE(outcome.download);
    EXPECT_EQ(model.ready()->version, (Version{1, 3, 0}));
}

} // namespace
