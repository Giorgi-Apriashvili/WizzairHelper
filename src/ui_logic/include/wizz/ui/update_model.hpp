#pragma once
// The self-updater's presentation rules (DECISIONS D-45..D-49), Qt-free. The
// spec has no updater; nothing here changes a specified behaviour.
//
//   * When the app may check at all: never in a self-test (--autotest) or replay
//     (--replay) run, so those runs stay offline and reproducible.
//   * What a finished check means for this copy: nothing when it is up to date or
//     no release is published; one Log line when the check failed; for a newer
//     release one "Update available" line, then an installed copy downloads and
//     verifies the installer (the core's download_verified) and a portable copy
//     offers the release page (no self-replacement of a portable folder).
//   * The update bar: hidden unless an update is ready or available; "Later"
//     hides it for the rest of the session; "Restart and update" is disabled while
//     a search or phase is working.
//   * The installer command the app runs when it closes: the contract's silent
//     command line for a verified installer, with /RELAUNCH after "Restart and
//     update".
//
// The Qt side (src/app/lib/update_controller.hpp) runs the core's check and
// download on worker threads, renders UpdateBarView and launches the command.

#include "wizz/services/updater.hpp"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wizz::ui {

/// False when `arguments` (the command line without the program path) contain
/// `--autotest` or `--replay` (exact, case-sensitive, anywhere, like the 01 §17.1
/// flags): self-test and replay runs never contact GitHub (D-45).
[[nodiscard]] bool updates_allowed(std::span<const std::string> arguments) noexcept;

namespace update_strings {

/// The update bar's buttons (D-46).
inline constexpr std::string_view kRestartAndUpdate = "Restart and update";
inline constexpr std::string_view kLater = "Later";
inline constexpr std::string_view kDownload = "Download";

/// "WizzairHelper X.Y.Z is ready to install. It will be installed when you close WizzairHelper."
[[nodiscard]] std::string ready_text(const update::Version& version);
/// "WizzairHelper X.Y.Z is available."
[[nodiscard]] std::string available_text(const update::Version& version);

// Log tab lines (D-48).
/// "Update available: WizzairHelper X.Y.Z (running A.B.C)."
[[nodiscard]] std::string available_log(const update::Version& latest, const update::Version& running);
/// "Update X.Y.Z downloaded and verified."
[[nodiscard]] std::string verified_log(const update::Version& version);
/// "Update X.Y.Z: installer started."
[[nodiscard]] std::string install_started_log(const update::Version& version);
/// "Update X.Y.Z: the installer could not be started: {error}"
[[nodiscard]] std::string install_failed_log(const update::Version& version, std::string_view error);
/// "Update check failed: {reason}", where the reason is the core's message without its
/// own leading "Update check failed: ".
[[nodiscard]] std::string check_failed_log(std::string_view message);
/// "Update download failed: {message}"
[[nodiscard]] std::string download_failed_log(std::string_view message);

} // namespace update_strings

/// What the update bar shows.
struct UpdateBarView {
    bool visible = false;
    std::string text;            ///< the message, plain text
    std::string action;          ///< "Restart and update" or "Download"
    bool action_enabled = false;

    friend bool operator==(const UpdateBarView&, const UpdateBarView&) = default;
};

/// An installer that was downloaded and verified (installed copies).
struct ReadyInstaller {
    update::Version version;
    std::filesystem::path path;

    friend bool operator==(const ReadyInstaller&, const ReadyInstaller&) = default;
};

/// A newer release a portable copy can download from its release page.
struct AvailableRelease {
    update::Version version;
    std::string page_url;  ///< the release's html_url

    friend bool operator==(const AvailableRelease&, const AvailableRelease&) = default;
};

/// The update state of one app session (UI thread only).
class UpdateModel {
public:
    UpdateModel(update::Version running, update::InstallKind kind) noexcept;

    struct CheckOutcome {
        std::vector<std::string> log;  ///< Log tab lines, in order
        bool download = false;         ///< download and verify pending_download()'s installer now
    };

    /// A daily check finished:
    /// - no_release (nothing published, a draft or a prerelease) or a release that is not
    ///   newer than the running version: nothing;
    /// - any other error: one check_failed_log line;
    /// - a newer release already ready / offered (or an older one): nothing;
    /// - a newer release: available_log once per version and session; a portable copy
    ///   offers its page (available()), an installed copy remembers it as
    ///   pending_download() and asks for the download (also again after an earlier
    ///   download of it failed).
    [[nodiscard]] CheckOutcome on_check(const std::expected<update::ReleaseInfo, update::UpdateError>& result);

    /// The download of pending_download()'s installer finished: on success it is ready()
    /// (verified_log); on failure download_failed_log, and a network or HTTP-status
    /// failure makes download_retry_due() true. Returns the Log tab lines.
    [[nodiscard]] std::vector<std::string> on_download(
        const std::expected<update::DownloadedFile, update::UpdateError>& result);

    /// A newer release whose installer is still to be downloaded and verified.
    [[nodiscard]] const std::optional<update::ReleaseInfo>& pending_download() const noexcept { return pending_; }
    /// The last download of pending_download() failed for a reason that may heal (no
    /// network, an HTTP status): the next re-evaluation downloads again instead of checking.
    [[nodiscard]] bool download_retry_due() const noexcept { return pending_.has_value() && retry_download_; }

    [[nodiscard]] const std::optional<ReadyInstaller>& ready() const noexcept { return ready_; }
    [[nodiscard]] const std::optional<AvailableRelease>& available() const noexcept { return available_; }

    /// "Later": the bar stays hidden for the rest of the session; a ready installer
    /// still runs when the app closes.
    void dismiss() noexcept { dismissed_ = true; }
    [[nodiscard]] bool dismissed() const noexcept { return dismissed_; }

    /// "Restart and update": accepted only with a ready installer and while nothing is
    /// working; the exit command then carries /RELAUNCH. Returns whether it was accepted.
    bool request_restart(bool working) noexcept;
    [[nodiscard]] bool restart_requested() const noexcept { return restart_; }

    /// The bar: "ready to install" with "Restart and update" (disabled while `working`),
    /// else "available" with "Download", else hidden; hidden after dismiss().
    [[nodiscard]] UpdateBarView bar(bool working) const;

    /// The command to run when the app closes: the contract's silent command for the
    /// ready installer — `/RELAUNCH` only after request_restart() — or nullopt when no
    /// verified installer is ready.
    [[nodiscard]] std::optional<update::InstallerCommand> exit_command() const;

    [[nodiscard]] const update::Version& running() const noexcept { return running_; }
    [[nodiscard]] update::InstallKind install_kind() const noexcept { return kind_; }

private:
    update::Version running_;
    update::InstallKind kind_;
    std::optional<update::Version> announced_;
    std::optional<update::ReleaseInfo> pending_;
    bool retry_download_ = false;
    std::optional<ReadyInstaller> ready_;
    std::optional<AvailableRelease> available_;
    bool dismissed_ = false;
    bool restart_ = false;
};

} // namespace wizz::ui
