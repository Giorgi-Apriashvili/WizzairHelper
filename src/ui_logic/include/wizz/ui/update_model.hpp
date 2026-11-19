#pragma once
// The self-updater's presentation rules (DECISIONS D-45..D-49, D-59..D-62), Qt-free.
// The spec has no updater; nothing here changes a specified behaviour.
//
//   * When the app may check at all: never in a self-test (--autotest) or replay
//     (--replay) run, so those runs stay offline and reproducible.
//   * The user's options (update_options.hpp): whether and how often the app checks on
//     its own (the schedule is the core's, fed by check_policy()), and what an
//     installed copy does with an update it found (WhenFound):
//       install_on_close   download and verify; the update bar offers "Restart and
//                          update"; closing the app installs it (2.0 / 2.1, default);
//       download_then_ask  download and verify; the bar offers "Restart and update";
//                          closing the app does NOT install it;
//       notify_only        no download; the bar says it is available and "Update now"
//                          downloads, verifies, then restarts and installs.
//     With automatic checks off nothing downloads on its own: a release that waits for
//     the user keeps "Update now" whatever the choice. A download queued behind other
//     work starts only if it is still wanted (download_wanted()).
//     A portable copy cannot install itself: it always offers the release page.
//   * What a finished check means: nothing visible when it is up to date or no release
//     is published; one Log line when it failed; for a newer release one "Update
//     available" line per version and session, then the choice above.
//   * An update put off in an earlier session (Later, notify only, closed under
//     download_then_ask, a portable copy, an installer that did not start) is offered
//     again at every start from what the app already knows — update-state.json's
//     LatestSeenVersion and a verified installer in the downloads folder — without any
//     request (remind()): a verified installer (hashed again from disk against the hash
//     kept when it was verified) is ready under install_on_close and download_then_ask;
//     otherwise, and under notify_only, "Update now" checks for its release first and then
//     downloads it (the user asked); a portable copy links to its release page. When a
//     check of the session finds that release, its download confirms the installer
//     against the release's SHA256SUMS.txt while the bar keeps offering it.
//   * The update bar (UpdateBarView) and what its action does (BarAction); "Later"
//     hides the bar for the rest of the session.
//   * The Settings window's "Updates" status (UpdateStatusView): the version and the
//     last successful check ("last checked", or "last successful check" after a failed
//     check), checking / up to date / available / downloading / ready / failed in plain
//     words, and the bar's action under Check now (D-74), so that the update a check
//     found is one click away without closing the window.
//   * The installer command the app runs when it closes.
//
// The Qt side (src/app/lib/update_controller.hpp) runs the core's checks and downloads
// on worker threads, renders the views and launches the command.

#include "wizz/core/locale.hpp"
#include "wizz/core/time.hpp"
#include "wizz/services/update_options.hpp"
#include "wizz/services/updater.hpp"

#include <cstdint>
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

using CheckResult = std::expected<update::ReleaseInfo, update::UpdateError>;
using DownloadResult = std::expected<update::DownloadedFile, update::UpdateError>;

namespace update_strings {

// --- The update bar (D-46, D-61) -----------------------------------------------------
inline constexpr std::string_view kRestartAndUpdate = "Restart and update";
inline constexpr std::string_view kLater = "Later";
/// A portable copy: opens the release page.
inline constexpr std::string_view kDownload = "Download";
/// An installed copy that only tells about updates: download, verify, restart, install.
inline constexpr std::string_view kUpdateNow = "Update now";

/// Install on close: "WizzairHelper X.Y.Z is ready to install. It will be installed when
/// you close WizzairHelper."
[[nodiscard]] std::string ready_text(const update::Version& version);
/// "WizzairHelper X.Y.Z is ready to install." (the bar of the other choices; the status line)
[[nodiscard]] std::string ready_to_install_text(const update::Version& version);
/// "WizzairHelper X.Y.Z is available."
[[nodiscard]] std::string available_text(const update::Version& version);
/// "Downloading WizzairHelper X.Y.Z…"
[[nodiscard]] std::string downloading_text(const update::Version& version);

// --- Log tab lines (D-48) -------------------------------------------------------------
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

// --- The Settings window (D-62) ---------------------------------------------------------
/// The window's title, and the gear button's accessible name and tooltip.
inline constexpr std::string_view kSettingsTitle = "Settings";
inline constexpr std::string_view kUpdatesSection = "Updates";
inline constexpr std::string_view kCheckNow = "Check now";
/// Check now while its check runs (busy and disabled).
inline constexpr std::string_view kCheckNowBusy = "Checking…";
inline constexpr std::string_view kClose = "Close";
inline constexpr std::string_view kCheckAutomatically = "Check for updates automatically";
/// The frequency drop-down's accessible name.
inline constexpr std::string_view kCheckFrequencyName = "How often to check";
inline constexpr std::string_view kWhenFoundCaption = "When an update is found:";
/// The reason the install choices are disabled in a portable copy.
inline constexpr std::string_view kPortableNote =
    "This portable copy cannot install updates itself; it links to the release page instead.";
inline constexpr std::string_view kChecking = "Checking for updates…";
inline constexpr std::string_view kUpToDate = "You have the latest version.";
/// The status of a run without updates (--autotest, --replay).
inline constexpr std::string_view kOffInThisRun = "Updates are off in this run.";

/// "Once a day", "Once a week", "Once a month".
[[nodiscard]] std::string_view frequency_label(update::CheckFrequency frequency) noexcept;
/// "Download it and install when I close the app", "Download it, then ask me before
/// installing", "Only tell me that an update is available".
[[nodiscard]] std::string_view when_found_label(update::WhenFound when_found) noexcept;

/// Which check the version line's time belongs to.
enum class LastCheck : std::uint8_t {
    succeeded,  ///< no check failed since the last one that worked: "last checked …"
    failed,     ///< the last check failed: the time is the last *successful* one's
};

/// "WizzairHelper 2.1.0 · last checked today at 15:53" — "yesterday at 09:05", "on Mon 16
/// Nov 2026 at 09:05" (`locale`'s "ddd dd MMM yyyy" and "HH:mm") — or "WizzairHelper 2.1.0
/// · never checked". After a failed check (LastCheck::failed) the time is named for what
/// it is: "WizzairHelper 2.1.0 · last successful check today at 10:00", or "WizzairHelper
/// 2.1.0 · no successful check yet". `last_success` is shown in the local time of `clock`.
[[nodiscard]] std::string version_line(const update::Version& running, std::optional<UtcInstant> last_success,
                                       const Clock& clock, const Locale& locale,
                                       LastCheck last_check = LastCheck::succeeded);
/// A failed check in plain words, e.g. "Could not reach GitHub. Check your internet
/// connection and try again."
[[nodiscard]] std::string check_failed_status(const update::UpdateError& error);
/// A failed download in plain words, e.g. "WizzairHelper 1.2.0 could not be downloaded
/// (HTTP 503). Try again later."
[[nodiscard]] std::string download_failed_status(const update::Version& version, const update::UpdateError& error);

} // namespace update_strings

/// What the update bar shows.
struct UpdateBarView {
    bool visible = false;
    std::string text;            ///< the message, plain text
    /// "Restart and update", "Update now" or "Download"; empty = no action button (while
    /// "Update now" downloads). "Later" shows whenever the bar does.
    std::string action;
    bool action_enabled = false;

    friend bool operator==(const UpdateBarView&, const UpdateBarView&) = default;
};

/// What the bar's action button asks the controller to do (UpdateModel::on_action).
enum class BarAction : std::uint8_t {
    none,               ///< nothing (disabled while working, or already under way)
    restart,            ///< close the window; the ready installer then starts with /RELAUNCH
    download,           ///< download and verify pending_download() now; on_download then restarts
    /// "Update now" for an update remembered from an earlier session (remembered()): check
    /// releases/latest first (begin_fetch, then on_check with CheckOrigin::update_now),
    /// whose answer asks for the download; on_download then restarts.
    check_then_download,
    open_release_page,  ///< a portable copy: open available()->page_url in the browser
};

/// Who asked for a check.
enum class CheckOrigin : std::uint8_t {
    automatic,   ///< the schedule (check_policy)
    manual,      ///< Check now in the Settings window
    update_now,  ///< "Update now" for a remembered update: the check that finds its release
};

/// The Settings window's status line.
enum class UpdateStatusKind : std::uint8_t {
    not_checked,  ///< never checked: no status line ("never checked" is in the version line)
    checking,     ///< "Checking for updates…"
    up_to_date,   ///< "You have the latest version."
    available,    ///< "WizzairHelper X.Y.Z is available."
    downloading,  ///< "Downloading WizzairHelper X.Y.Z…"
    ready,        ///< "WizzairHelper X.Y.Z is ready to install."
    failed,       ///< the last check or download failed, in plain words
    off,          ///< "Updates are off in this run." (--autotest, --replay)
};

/// The "Updates" section of the Settings window.
struct UpdateStatusView {
    UpdateStatusKind kind = UpdateStatusKind::not_checked;
    std::string version_line;  ///< "WizzairHelper 2.1.0 · last checked today at 15:53"
    std::string status;        ///< the status line; empty for not_checked
    bool checking = false;     ///< Check now shows that it is busy
    bool check_now_enabled = true;    ///< false while checking or downloading, and in an `off` run
    bool options_enabled = true;      ///< the check box and the drop-down; false in an `off` run
    bool when_found_enabled = true;   ///< the three install choices; false for a portable copy or an `off` run
    std::string when_found_note;      ///< kPortableNote for a portable copy, else empty
    /// The button under Check now (D-74): the update bar's action — "Restart and update",
    /// "Update now" or "Download" — also after "Later" hid the bar; empty = none: nothing on
    /// offer, or the status is `downloading` (a download under way — "Update now"'s, or a
    /// newer release's while the bar still offers an older verified installer).
    std::string action;
    /// As in the bar: "Restart and update" and "Update now" are disabled while a search or
    /// phase is working; "Download" (a portable copy's release page) is not.
    bool action_enabled = false;

    friend bool operator==(const UpdateStatusView&, const UpdateStatusView&) = default;
};

/// The view of a run without updates (no UpdateController): the version, "Updates are
/// off in this run.", every control disabled.
[[nodiscard]] UpdateStatusView updates_off_view(const update::Version& running);

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

/// What the app knows at start-up, without asking GitHub, of an update found in an
/// earlier session (UpdateModel::remind).
struct EarlierUpdate {
    update::Version version;  ///< update-state.json's LatestSeenVersion
    std::string page_url;     ///< its release page (UpdateConfig::release_page_url), for a portable copy
    /// Its verified installer in the downloads folder (update::find_verified_download), for
    /// an installed copy; nullopt when there is none.
    std::optional<std::filesystem::path> installer;

    friend bool operator==(const EarlierUpdate&, const EarlierUpdate&) = default;
};

/// The update state of one app session (UI thread only). See the file comment and
/// DECISIONS D-61 for the state machine.
class UpdateModel {
public:
    UpdateModel(update::Version running, update::InstallKind kind, update::UpdateOptions options = {}) noexcept;

    // --- Options -----------------------------------------------------------------------
    [[nodiscard]] const update::UpdateOptions& options() const noexcept { return options_; }
    /// The Settings window changed the options; they apply at once. Returns true when a
    /// download should start now: an installed copy switched to a choice that downloads
    /// (install_on_close, download_then_ask) with automatic checks on, while a newer
    /// release is known whose download was never tried in this session (a notify-only
    /// offer). Options that no longer download on their own (notify_only, or automatic
    /// checks off) withdraw such a download that has not started yet (download_wanted()).
    /// Switching to install_on_close with a verified installer makes closing the app
    /// install it; switching away makes closing leave it.
    [[nodiscard]] bool set_options(const update::UpdateOptions& options) noexcept;
    /// The automatic check's policy: the options' switch and frequency (D-59).
    [[nodiscard]] update::CheckPolicy check_policy() const;

    // --- update-state.json, for the version line --------------------------------------
    void set_saved_state(const update::UpdateState& state) { saved_ = state; }
    [[nodiscard]] const update::UpdateState& saved_state() const noexcept { return saved_; }

    // --- The reminder at start-up ------------------------------------------------------
    /// The session's first evaluation: an update found in an earlier session and not
    /// installed is offered again, from what the app knows — nothing here asks GitHub.
    /// Ignored when `earlier.version` is not newer than the running version, or when a
    /// check of this session has already answered. Otherwise the version is announced
    /// (available_log, once per session) and remembered():
    /// - portable: available() with `earlier.page_url` ("Download");
    /// - installed with `earlier.installer` and a choice that downloads (install_on_close,
    ///   download_then_ask): ready() — "Restart and update", installed on close under
    ///   install_on_close;
    /// - installed otherwise (no verified installer, or notify_only): available_text +
    ///   "Update now", which checks for the release first (BarAction::check_then_download).
    /// The offer follows later changes of the choice and never downloads on its own; the
    /// next successful check replaces it with what it found. Returns the Log lines.
    [[nodiscard]] std::vector<std::string> remind(const EarlierUpdate& earlier);
    /// The update remembered from an earlier session that no check of this session has
    /// answered yet (remind()).
    [[nodiscard]] const std::optional<EarlierUpdate>& remembered() const noexcept { return remembered_; }

    // --- Checks ------------------------------------------------------------------------
    /// Check now was pressed: the status says "Checking for updates…" until on_check with
    /// CheckOrigin::manual.
    void begin_manual_check() noexcept { checking_ = true; }
    [[nodiscard]] bool checking() const noexcept { return checking_; }

    struct CheckOutcome {
        std::vector<std::string> log;  ///< Log tab lines, in order
        bool download = false;         ///< download and verify pending_download()'s installer now
    };

    /// A check finished:
    /// - no_release (nothing published, a draft or a prerelease) or a release that is not
    ///   newer than the running version: nothing visible (the status: up to date), and a
    ///   remembered() update is dropped;
    /// - any other error: one check_failed_log line (the status: the failure; the version
    ///   line names the last *successful* check); a remembered() update stays offered;
    /// - a release, after a remembered() update: the check's answer replaces it. Under a
    ///   choice that downloads its verified installer stays offered as ready(): for that
    ///   very release while the release's download (asked for as usual) confirms it against
    ///   the release's SHA256SUMS.txt — download_verified reuses the file when it matches;
    ///   if that download fails it is no longer offered — and for a newer release while the
    ///   newer one downloads;
    /// - a newer release already ready, offered or being downloaded (or an older one):
    ///   nothing new;
    /// - a newer release: available_log once per version and session; a portable copy
    ///   offers its page (available()); an installed copy remembers it as
    ///   pending_download() and asks for the download unless the choice is notify_only
    ///   (also again after an earlier download of it failed) — or "Update now" asked for
    ///   it (install_requested()).
    /// A manual check that finds a newer release shows the bar again after "Later". The
    /// check of "Update now" for a remembered update (CheckOrigin::update_now) that fails
    /// or finds nothing newer ends that request: "Update now" is offered again, or nothing.
    [[nodiscard]] CheckOutcome on_check(const CheckResult& result, CheckOrigin origin = CheckOrigin::automatic);

    /// The check of "Update now" for a remembered update starts (the bar and the status:
    /// downloading).
    void begin_fetch() noexcept { fetching_ = true; }
    [[nodiscard]] bool fetching() const noexcept { return fetching_; }
    /// "Update now" asked to check for a remembered update's release, that check has not
    /// started, and nothing made it unnecessary meanwhile ("Later", or another check that
    /// answered first). The controller asks before it starts a queued one.
    [[nodiscard]] bool fetch_wanted() const noexcept;

    // --- Downloads ---------------------------------------------------------------------
    /// The download of pending_download() starts (the status: downloading).
    void begin_download() noexcept;
    [[nodiscard]] bool downloading() const noexcept { return downloading_; }

    struct DownloadOutcome {
        std::vector<std::string> log;  ///< Log tab lines, in order
        bool restart = false;          ///< "Update now" is complete: close the window and install
    };

    /// The download of pending_download()'s installer finished. Success: it is ready()
    /// (verified_log); after "Update now" the app restarts and installs at once unless a
    /// search or phase is `working` (then the bar offers "Restart and update"). Failure:
    /// download_failed_log, the status names it, "Update now" is offered again, and a
    /// network or HTTP-status failure makes download_retry_due() true.
    [[nodiscard]] DownloadOutcome on_download(const DownloadResult& result, bool working);

    /// A check or download ended with an unexpected exception (logged by the caller like a
    /// failed check): nothing is checking or downloading any more, "Update now" is offered
    /// again, and the status says "Something went wrong: {what}". It counts as a failed
    /// check for the version line ("last successful check") when it ended Check now, the
    /// check of "Update now", or — `during_check`, which the caller knows — an automatic
    /// check.
    void on_unexpected_error(std::string_view what, bool during_check = false);

    /// A newer release whose installer is still to be downloaded and verified.
    [[nodiscard]] const std::optional<update::ReleaseInfo>& pending_download() const noexcept { return pending_; }
    /// A download of pending_download() that was asked for and has not started is still
    /// wanted: "Update now" was pressed (and not withdrawn by "Later"), or on_check /
    /// set_options asked for it and the options still download on their own. The
    /// controller asks before it starts a queued download.
    [[nodiscard]] bool download_wanted() const noexcept;
    /// The last automatic download of pending_download() failed for a reason that may heal
    /// (no network, an HTTP status), automatic checks are on and the choice downloads: the
    /// next re-evaluation downloads again instead of checking.
    [[nodiscard]] bool download_retry_due() const noexcept;

    /// The verified installer on offer: the one verified in this session, else an earlier
    /// session's (remembered(), or one whose release's download is confirming it) while
    /// the choice downloads (install_on_close, download_then_ask).
    [[nodiscard]] std::optional<ReadyInstaller> ready() const;
    [[nodiscard]] const std::optional<AvailableRelease>& available() const noexcept { return available_; }

    // --- The update bar ------------------------------------------------------------------
    /// The bar (hidden after dismiss()):
    /// - portable, a newer release: available_text + "Download";
    /// - installed, a newer release not yet verified that waits for the user — the choice
    ///   is notify_only, or automatic checks are off and no download of it runs or was
    ///   asked for (a notify-only offer whose choice then changed, or a failed download):
    ///   available_text + "Update now" (disabled while `working`); after it was pressed:
    ///   downloading_text, no action;
    /// - installed, a verified installer: ready_text (install_on_close) or
    ///   ready_to_install_text (the other choices, or after "Restart and update") +
    ///   "Restart and update" (disabled while `working`);
    /// - installed, a remembered() update without a ready() installer: available_text +
    ///   "Update now" as above; after it was pressed downloading_text, no action;
    /// - installed, a release downloading on its own (or about to) in a session that offered
    ///   an update again at start (remind()): downloading_text, no action, so that offer
    ///   does not vanish until the download is verified;
    /// - otherwise hidden (an installed copy downloading on its own shows nothing yet).
    [[nodiscard]] UpdateBarView bar(bool working) const;
    /// The bar's action button, or the same button of the Settings window (D-74): what
    /// bar() offers — also after "Later" — unless it is disabled while `working`.
    /// "Restart and update" sets restart_requested(); "Update now" sets install_requested()
    /// (none when its download is already running: it restarts when verified; for a
    /// remembered() update check_then_download) and, pressed in the Settings window after
    /// "Later", shows the bar again with the download's progress.
    [[nodiscard]] BarAction on_action(bool working);
    /// "Later": the bar stays hidden for the rest of the session (until a manual check
    /// finds a newer release; the next start reminds again, remind()) and a pending
    /// "Update now" no longer restarts (its check or download still runs if it has
    /// started, and is dropped if it has not); a ready installer still runs on exit with
    /// install_on_close.
    void dismiss() noexcept;
    [[nodiscard]] bool dismissed() const noexcept { return dismissed_; }
    [[nodiscard]] bool restart_requested() const noexcept { return restart_; }
    [[nodiscard]] bool install_requested() const noexcept { return install_requested_; }

    // --- Exit --------------------------------------------------------------------------
    /// The command to run when the app closes: the contract's silent command for the ready
    /// installer with /RELAUNCH after a restart request, without it when the choice is
    /// install_on_close; nullopt otherwise (no verified installer, a portable copy, or
    /// download_then_ask / notify_only without a restart request).
    [[nodiscard]] std::optional<update::InstallerCommand> exit_command() const;

    // --- The Settings window --------------------------------------------------------------
    /// The "Updates" section; `clock` and `locale` format the last successful check of
    /// saved_state(), named "last successful check" while the last check of this session
    /// failed (update_strings::LastCheck::failed), else "last checked". The status, first
    /// that applies: checking; downloading (also the check of "Update now" for a
    /// remembered update; not the download that only confirms the installer the bar offers
    /// as ready); failed when Check now failed (the user asked: also over a ready
    /// installer, until anything else happens); ready (a verified installer, unless
    /// "Update now" offers a newer release); failed (the last check or download failed);
    /// available (also a remembered update); up to date (a check this session found
    /// nothing newer); before any check this session, from saved_state(): available when
    /// it saw a newer version, up to date after a successful check, else not checked.
    /// The action is the bar's (bar(working)) whether or not "Later" hid the bar, and none
    /// while the status is downloading (D-74).
    [[nodiscard]] UpdateStatusView status_view(const Clock& clock, const Locale& locale, bool working = false) const;

    [[nodiscard]] const update::Version& running() const noexcept { return running_; }
    [[nodiscard]] update::InstallKind install_kind() const noexcept { return kind_; }

private:
    enum class Offer : std::uint8_t { none, ready, release_page, update_now, downloading_for_install };

    /// A failed check or download, in plain words.
    struct Problem {
        std::string text;
        /// The failure of Check now: the status shows it also over a ready installer,
        /// since the user asked (until anything else happens).
        bool answers_check_now = false;
    };

    [[nodiscard]] Offer offer() const noexcept;
    /// What bar() shows when "Later" has not hidden it.
    [[nodiscard]] UpdateBarView offer_view(bool working) const;
    /// on_check without its bookkeeping of the checks in flight.
    [[nodiscard]] CheckOutcome take_check(const CheckResult& result, CheckOrigin origin);
    /// pending_ is newer than any ready installer.
    [[nodiscard]] bool newer_pending() const noexcept;
    /// The remembered update's verified installer is on offer as ready().
    [[nodiscard]] bool remembered_ready() const noexcept;
    /// confirming_ is on offer as ready() (the choice downloads, like remembered_ready()).
    [[nodiscard]] bool confirming_offered() const noexcept;
    /// ready() has a value.
    [[nodiscard]] bool has_ready() const noexcept;
    /// The version "Update now" (or its download) is about: pending_'s when newer_pending(),
    /// else the remembered one's.
    [[nodiscard]] update::Version wanted_version() const noexcept;
    /// A check of this session answered with `found` (nullptr: no newer release): the
    /// remembered update gives way to it.
    void forget_remembered(const update::ReleaseInfo* found);
    [[nodiscard]] bool downloads_on_its_own() const noexcept;
    /// Nothing downloads pending_ unless the user presses "Update now" (see bar()).
    [[nodiscard]] bool waits_for_user() const noexcept;

    update::Version running_;
    update::InstallKind kind_;
    update::UpdateOptions options_;
    update::UpdateState saved_;
    std::optional<update::Version> announced_;
    std::optional<update::ReleaseInfo> pending_;
    bool download_tried_ = false;  ///< a download of pending_ was started in this session
    bool download_asked_ = false;  ///< on_check or set_options asked for a download of pending_ not yet started
    bool downloading_ = false;
    bool retry_download_ = false;
    std::optional<ReadyInstaller> ready_;
    /// remembered_'s verified installer after a check found its very release: still offered
    /// as ready while that release's download confirms it against its SHA256SUMS.txt
    /// (download_verified reuses a file that matches, D-43); dropped if that fails.
    std::optional<ReadyInstaller> confirming_;
    std::optional<AvailableRelease> available_;
    std::optional<Problem> problem_;  ///< the last check or download failure
    std::optional<EarlierUpdate> remembered_;  ///< remind()'s update until a check of this session answers
    bool reminded_ = false;           ///< remind() offered an update in this session
    bool checked_ = false;            ///< a check of this session answered (a release or no_release)
    bool last_check_failed_ = false;  ///< the last check of this session failed
    bool fetching_ = false;           ///< the check of "Update now" for remembered_ runs
    bool checking_ = false;
    bool up_to_date_ = false;  ///< the last successful check found nothing newer
    bool dismissed_ = false;
    bool restart_ = false;
    bool install_requested_ = false;
};

} // namespace wizz::ui
