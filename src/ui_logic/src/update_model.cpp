#include "wizz/ui/update_model.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace wizz::ui {

namespace {

/// The prefix of the core's own check-failure messages (updater.cpp).
constexpr std::string_view kCheckFailedPrefix = "Update check failed: ";

/// Failures that may heal within the session: no network, a timeout, an HTTP status.
[[nodiscard]] constexpr bool may_heal(update::UpdateErrorKind kind) noexcept
{
    return kind == update::UpdateErrorKind::network || kind == update::UpdateErrorKind::http_status;
}

/// GitHub's answers to too many anonymous requests.
[[nodiscard]] constexpr bool is_rate_limit(int status) noexcept
{
    return status == 403 || status == 429;
}

} // namespace

bool updates_allowed(std::span<const std::string> arguments) noexcept
{
    return std::ranges::none_of(arguments, [](const std::string& argument) {
        return argument == "--autotest" || argument == "--replay";
    });
}

namespace update_strings {

std::string ready_text(const update::Version& version)
{
    return std::format("WizzairHelper {} is ready to install. It will be installed when you close WizzairHelper.",
                       version.to_string());
}

std::string ready_to_install_text(const update::Version& version)
{
    return std::format("WizzairHelper {} is ready to install.", version.to_string());
}

std::string available_text(const update::Version& version)
{
    return std::format("WizzairHelper {} is available.", version.to_string());
}

std::string downloading_text(const update::Version& version)
{
    return std::format("Downloading WizzairHelper {}…", version.to_string());
}

std::string available_log(const update::Version& latest, const update::Version& running)
{
    return std::format("Update available: WizzairHelper {} (running {}).", latest.to_string(), running.to_string());
}

std::string verified_log(const update::Version& version)
{
    return std::format("Update {} downloaded and verified.", version.to_string());
}

std::string install_started_log(const update::Version& version)
{
    return std::format("Update {}: installer started.", version.to_string());
}

std::string install_failed_log(const update::Version& version, std::string_view error)
{
    return std::format("Update {}: the installer could not be started: {}", version.to_string(), error);
}

std::string check_failed_log(std::string_view message)
{
    if (message.starts_with(kCheckFailedPrefix)) {
        message.remove_prefix(kCheckFailedPrefix.size());
    }
    return std::format("{}{}", kCheckFailedPrefix, message);
}

std::string download_failed_log(std::string_view message)
{
    return std::format("Update download failed: {}", message);
}

std::string_view frequency_label(update::CheckFrequency frequency) noexcept
{
    switch (frequency) {
    case update::CheckFrequency::daily:
        return "Once a day";
    case update::CheckFrequency::weekly:
        return "Once a week";
    case update::CheckFrequency::monthly:
        return "Once a month";
    }
    return "Once a day";
}

std::string_view when_found_label(update::WhenFound when_found) noexcept
{
    switch (when_found) {
    case update::WhenFound::install_on_close:
        return "Download it and install when I close the app";
    case update::WhenFound::download_then_ask:
        return "Download it, then ask me before installing";
    case update::WhenFound::notify_only:
        return "Only tell me that an update is available";
    }
    return "Download it and install when I close the app";
}

std::string version_line(const update::Version& running, std::optional<UtcInstant> last_success, const Clock& clock,
                         const Locale& locale, LastCheck last_check)
{
    const std::string head = std::format("WizzairHelper {} · ", running.to_string());
    const bool after_failure = last_check == LastCheck::failed;
    if (!last_success) {
        return head + (after_failure ? "no successful check yet" : "never checked");
    }
    // After a failed check the time is not the last check's: say whose it is.
    const std::string_view what = after_failure ? "last successful check" : "last checked";
    const LocalDateTime when = clock.to_local(*last_success);
    const Date day = date_of(when);
    const Date today = date_of(clock.to_local(clock.utc_now()));
    const std::string time = locale.format_time(when, "HH:mm");
    if (day == today) {
        return std::format("{}{} today at {}", head, what, time);
    }
    if (day == today.add_days(-1)) {
        return std::format("{}{} yesterday at {}", head, what, time);
    }
    return std::format("{}{} on {} at {}", head, what, locale.format_date(day, "ddd dd MMM yyyy"), time);
}

std::string check_failed_status(const update::UpdateError& error)
{
    switch (error.kind) {
    case update::UpdateErrorKind::network:
        return "Could not reach GitHub. Check your internet connection and try again.";
    case update::UpdateErrorKind::http_status:
        if (error.http_status && is_rate_limit(*error.http_status)) {
            return std::format("GitHub is limiting requests right now (HTTP {}). Try again later.", *error.http_status);
        }
        if (error.http_status) {
            return std::format("GitHub could not answer the update check (HTTP {}). Try again later.",
                               *error.http_status);
        }
        return "GitHub could not answer the update check. Try again later.";
    case update::UpdateErrorKind::no_release:
    case update::UpdateErrorKind::bad_release:
    case update::UpdateErrorKind::missing_asset:
    case update::UpdateErrorKind::bad_checksums:
    case update::UpdateErrorKind::checksum_mismatch:
    case update::UpdateErrorKind::file_system:
        break;
    }
    return "The latest release on GitHub could not be read. Try again later.";
}

std::string download_failed_status(const update::Version& version, const update::UpdateError& error)
{
    const std::string name = std::format("WizzairHelper {}", version.to_string());
    switch (error.kind) {
    case update::UpdateErrorKind::network:
        return std::format("{} could not be downloaded. Check your internet connection and try again.", name);
    case update::UpdateErrorKind::http_status:
        if (error.http_status) {
            return std::format("{} could not be downloaded (HTTP {}). Try again later.", name, *error.http_status);
        }
        return std::format("{} could not be downloaded. Try again later.", name);
    case update::UpdateErrorKind::checksum_mismatch:
        return std::format("The download of {} did not match its checksum and was deleted. Try again later.", name);
    case update::UpdateErrorKind::file_system:
        return std::format("{} could not be saved on this computer.", name);
    case update::UpdateErrorKind::no_release:
    case update::UpdateErrorKind::bad_release:
    case update::UpdateErrorKind::missing_asset:
    case update::UpdateErrorKind::bad_checksums:
        break;
    }
    return std::format("The {} release on GitHub is incomplete. Try again later.", name);
}

} // namespace update_strings

UpdateStatusView updates_off_view(const update::Version& running)
{
    return UpdateStatusView{.kind = UpdateStatusKind::off,
                            .version_line = std::format("WizzairHelper {}", running.to_string()),
                            .status = std::string{update_strings::kOffInThisRun},
                            .checking = false,
                            .check_now_enabled = false,
                            .options_enabled = false,
                            .when_found_enabled = false,
                            .when_found_note = {}};
}

UpdateModel::UpdateModel(update::Version running, update::InstallKind kind, update::UpdateOptions options) noexcept
    : running_(running), kind_(kind), options_(options)
{
}

bool UpdateModel::downloads_on_its_own() const noexcept
{
    return kind_ == update::InstallKind::installed && options_.when_found != update::WhenFound::notify_only;
}

bool UpdateModel::newer_pending() const noexcept
{
    return pending_.has_value() && (!ready_ || pending_->version > ready_->version);
}

bool UpdateModel::remembered_ready() const noexcept
{
    // A verified installer of an earlier session is offered as ready only under a choice
    // that downloads; notify only offers "Update now" (the owner's rule, D-61).
    return remembered_.has_value() && remembered_->installer.has_value() && downloads_on_its_own();
}

update::Version UpdateModel::wanted_version() const noexcept
{
    if (newer_pending()) {
        return pending_->version;
    }
    return remembered_ ? remembered_->version : update::Version{};
}

std::optional<ReadyInstaller> UpdateModel::ready() const
{
    if (ready_) {
        return ready_;
    }
    if (remembered_ready()) {
        return ReadyInstaller{.version = remembered_->version, .path = *remembered_->installer};
    }
    return std::nullopt;
}

std::vector<std::string> UpdateModel::remind(const EarlierUpdate& earlier)
{
    std::vector<std::string> log;
    // A version that is not newer is never offered; a check of this session knows better.
    if (earlier.version <= running_ || checked_ || remembered_) {
        return log;
    }
    remembered_ = earlier;
    if (kind_ == update::InstallKind::portable) {
        remembered_->installer.reset();  // a portable copy never installs
        available_ = AvailableRelease{.version = earlier.version, .page_url = earlier.page_url};
    }
    if (!announced_ || earlier.version > *announced_) {
        announced_ = earlier.version;
        log.push_back(update_strings::available_log(earlier.version, running_));
    }
    return log;
}

void UpdateModel::forget_remembered(const update::ReleaseInfo* found)
{
    checked_ = true;
    if (!remembered_) {
        return;
    }
    const EarlierUpdate earlier = *std::exchange(remembered_, std::nullopt);
    if (kind_ == update::InstallKind::portable) {
        if (available_ && available_->version == earlier.version) {
            available_.reset();  // the check's answer decides what is offered now
        }
        return;
    }
    // What the bar already offered as ready stays ready when the release is still there.
    if (found != nullptr && earlier.installer && found->version >= earlier.version && downloads_on_its_own() &&
        !ready_) {
        ready_ = ReadyInstaller{.version = earlier.version, .path = *earlier.installer};
    }
}

bool UpdateModel::fetch_wanted() const noexcept
{
    return kind_ == update::InstallKind::installed && remembered_.has_value() && install_requested_ && !fetching_ &&
           !newer_pending();
}

bool UpdateModel::waits_for_user() const noexcept
{
    if (options_.when_found == update::WhenFound::notify_only) {
        return true;
    }
    // A choice that downloads, with automatic checks off: nothing downloads it on its own
    // (D-61), so the offer keeps its "Update now".
    return !options_.check_automatically && !downloading_ && !download_asked_;
}

bool UpdateModel::set_options(const update::UpdateOptions& options) noexcept
{
    options_ = options;
    const bool downloads = downloads_on_its_own() && options_.check_automatically;
    if (!downloads) {
        download_asked_ = false;  // a download asked for and not started yet is withdrawn
    }
    const bool now = downloads && newer_pending() && !download_tried_ && !downloading_ && !install_requested_;
    download_asked_ = download_asked_ || now;
    return now;
}

update::CheckPolicy UpdateModel::check_policy() const
{
    return update::CheckPolicy{.automatic = options_.check_automatically, .frequency = options_.check_frequency};
}

bool UpdateModel::download_wanted() const noexcept
{
    return pending_.has_value() && !downloading_ && (install_requested_ || download_asked_);
}

UpdateModel::CheckOutcome UpdateModel::on_check(const CheckResult& result, CheckOrigin origin)
{
    if (origin == CheckOrigin::manual) {
        checking_ = false;
    }
    if (origin == CheckOrigin::update_now) {
        fetching_ = false;
    }
    CheckOutcome outcome = take_check(result, origin);
    if (origin == CheckOrigin::update_now && !outcome.download) {
        install_requested_ = false;  // failed or nothing newer: "Update now" is offered again, or nothing
    }
    if (install_requested_ && !remembered_ && !newer_pending()) {
        install_requested_ = false;  // nothing is left for "Update now" to download
    }
    return outcome;
}

UpdateModel::CheckOutcome UpdateModel::take_check(const CheckResult& result, CheckOrigin origin)
{
    CheckOutcome outcome;
    if (!result) {
        if (result.error().kind == update::UpdateErrorKind::no_release) {
            up_to_date_ = true;  // nothing is published to update to
            problem_.reset();
            last_check_failed_ = false;
            forget_remembered(nullptr);
            return outcome;
        }
        outcome.log.push_back(update_strings::check_failed_log(result.error().message));
        problem_ = update_strings::check_failed_status(result.error());
        last_check_failed_ = true;  // the version line names the last successful one; a remembered update stays
        return outcome;
    }
    problem_.reset();
    last_check_failed_ = false;
    const update::ReleaseInfo& release = *result;
    const bool newer = release.newer && release.version > running_;
    forget_remembered(newer ? &release : nullptr);
    if (!newer) {
        up_to_date_ = true;
        return outcome;
    }
    up_to_date_ = false;
    if (origin == CheckOrigin::manual) {
        dismissed_ = false;  // the user asked: show what there is again
    }
    // Nothing new when this version (or a newer one) is already ready, offered or on its way.
    if ((ready_ && release.version <= ready_->version) || (available_ && release.version <= available_->version) ||
        (pending_ && release.version < pending_->version) ||
        (pending_ && release.version == pending_->version && (downloading_ || install_requested_))) {
        return outcome;
    }
    if (!announced_ || release.version > *announced_) {
        announced_ = release.version;
        outcome.log.push_back(update_strings::available_log(release.version, running_));
    }
    if (kind_ == update::InstallKind::portable) {
        available_ = AvailableRelease{.version = release.version, .page_url = release.html_url};
        return outcome;
    }
    if (!pending_ || pending_->version != release.version) {
        download_tried_ = false;
    }
    pending_ = release;
    retry_download_ = false;
    download_asked_ = downloads_on_its_own();
    outcome.download = download_asked_ || install_requested_;  // "Update now" of a remembered update asked for it
    return outcome;
}

void UpdateModel::begin_download() noexcept
{
    downloading_ = true;
    download_tried_ = true;
    download_asked_ = false;
    problem_.reset();
}

bool UpdateModel::download_retry_due() const noexcept
{
    return pending_.has_value() && retry_download_ && !downloading_ && options_.check_automatically &&
           downloads_on_its_own();
}

UpdateModel::DownloadOutcome UpdateModel::on_download(const DownloadResult& result, bool working)
{
    DownloadOutcome outcome;
    downloading_ = false;
    download_asked_ = false;
    if (!pending_) {
        return outcome;
    }
    if (!result) {
        outcome.log.push_back(update_strings::download_failed_log(result.error().message));
        problem_ = update_strings::download_failed_status(pending_->version, result.error());
        retry_download_ = may_heal(result.error().kind);
        install_requested_ = false;  // "Update now" is offered again
        return outcome;
    }
    ready_ = ReadyInstaller{.version = pending_->version, .path = result->path};
    outcome.log.push_back(update_strings::verified_log(pending_->version));
    pending_.reset();
    retry_download_ = false;
    problem_.reset();
    if (std::exchange(install_requested_, false) && !working) {
        restart_ = true;  // "Update now": restart and install at once
        outcome.restart = true;
    }
    return outcome;
}

void UpdateModel::on_unexpected_error(std::string_view what)
{
    if (checking_ || fetching_) {
        last_check_failed_ = true;  // a check that was waited for ended without an answer
    }
    checking_ = false;
    fetching_ = false;
    downloading_ = false;
    download_asked_ = false;
    install_requested_ = false;
    problem_ = std::format("Something went wrong: {}", what);
}

UpdateModel::Offer UpdateModel::offer() const noexcept
{
    if (kind_ == update::InstallKind::portable) {
        return available_ ? Offer::release_page : Offer::none;
    }
    if (newer_pending() && install_requested_) {
        return Offer::downloading_for_install;
    }
    if (newer_pending() && waits_for_user()) {
        return Offer::update_now;
    }
    if (ready_ || remembered_ready()) {
        return Offer::ready;
    }
    if (remembered_) {  // an earlier session's update without a verified installer on offer
        return install_requested_ ? Offer::downloading_for_install : Offer::update_now;
    }
    return Offer::none;
}

UpdateBarView UpdateModel::bar(bool working) const
{
    if (dismissed_) {
        return {};
    }
    switch (offer()) {
    case Offer::none:
        return {};
    case Offer::ready: {
        const update::Version version = ready()->version;
        const bool on_close = options_.when_found == update::WhenFound::install_on_close && !restart_;
        return UpdateBarView{.visible = true,
                             .text = on_close ? update_strings::ready_text(version)
                                              : update_strings::ready_to_install_text(version),
                             .action = std::string{update_strings::kRestartAndUpdate},
                             .action_enabled = !working};
    }
    case Offer::release_page:
        return UpdateBarView{.visible = true,
                             .text = update_strings::available_text(available_->version),
                             .action = std::string{update_strings::kDownload},
                             .action_enabled = true};
    case Offer::update_now:
        return UpdateBarView{.visible = true,
                             .text = update_strings::available_text(wanted_version()),
                             .action = std::string{update_strings::kUpdateNow},
                             .action_enabled = !working};
    case Offer::downloading_for_install:
        return UpdateBarView{.visible = true,
                             .text = update_strings::downloading_text(wanted_version()),
                             .action = {},
                             .action_enabled = false};
    }
    return {};
}

BarAction UpdateModel::on_action(bool working)
{
    switch (offer()) {
    case Offer::ready:
        if (working) {
            return BarAction::none;
        }
        restart_ = true;
        return BarAction::restart;
    case Offer::release_page:
        return BarAction::open_release_page;
    case Offer::update_now:
        if (working) {
            return BarAction::none;
        }
        install_requested_ = true;
        if (!newer_pending()) {
            return BarAction::check_then_download;  // a remembered update: its release is checked for first
        }
        return downloading_ ? BarAction::none : BarAction::download;  // a running download restarts when verified
    case Offer::none:
    case Offer::downloading_for_install:
        break;
    }
    return BarAction::none;
}

void UpdateModel::dismiss() noexcept
{
    dismissed_ = true;
    install_requested_ = false;
}

std::optional<update::InstallerCommand> UpdateModel::exit_command() const
{
    const std::optional<ReadyInstaller> installer = ready();
    if (!installer || kind_ != update::InstallKind::installed) {
        return std::nullopt;
    }
    if (restart_) {
        return update::silent_install_command(installer->path, update::Relaunch::yes);
    }
    if (options_.when_found == update::WhenFound::install_on_close) {
        return update::silent_install_command(installer->path, update::Relaunch::no);
    }
    return std::nullopt;
}

UpdateStatusView UpdateModel::status_view(const Clock& clock, const Locale& locale) const
{
    UpdateStatusView view;
    view.version_line = update_strings::version_line(running_, saved_.last_success, clock, locale,
                                                     last_check_failed_ ? update_strings::LastCheck::failed
                                                                        : update_strings::LastCheck::succeeded);
    view.checking = checking_;
    view.check_now_enabled = !checking_ && !downloading_ && !fetching_;
    view.options_enabled = true;
    view.when_found_enabled = kind_ == update::InstallKind::installed;
    view.when_found_note = kind_ == update::InstallKind::portable ? std::string{update_strings::kPortableNote} : "";

    const auto set = [&view](UpdateStatusKind kind, std::string text) {
        view.kind = kind;
        view.status = std::move(text);
    };
    // "Update now" offers a release newer than a verified one: the status names the newer.
    const Offer current = offer();
    const bool offers_newer = current == Offer::update_now || current == Offer::downloading_for_install;
    const std::optional<ReadyInstaller> installer = ready();
    if (checking_) {
        set(UpdateStatusKind::checking, std::string{update_strings::kChecking});
    } else if (downloading_ && pending_) {
        set(UpdateStatusKind::downloading, update_strings::downloading_text(pending_->version));
    } else if (fetching_ && remembered_) {
        set(UpdateStatusKind::downloading, update_strings::downloading_text(remembered_->version));
    } else if (installer && !offers_newer) {
        set(UpdateStatusKind::ready, update_strings::ready_to_install_text(installer->version));
    } else if (problem_) {
        set(UpdateStatusKind::failed, *problem_);
    } else if (pending_ || available_ || remembered_) {
        const update::Version version = pending_     ? pending_->version
                                        : available_ ? available_->version
                                                     : remembered_->version;
        set(UpdateStatusKind::available, update_strings::available_text(version));
    } else if (up_to_date_) {
        set(UpdateStatusKind::up_to_date, std::string{update_strings::kUpToDate});
    } else if (saved_.latest_seen && *saved_.latest_seen > running_) {
        set(UpdateStatusKind::available, update_strings::available_text(*saved_.latest_seen));
    } else if (saved_.last_success) {
        set(UpdateStatusKind::up_to_date, std::string{update_strings::kUpToDate});
    }
    return view;
}

} // namespace wizz::ui
