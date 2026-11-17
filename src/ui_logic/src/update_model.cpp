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
                         const Locale& locale)
{
    const std::string head = std::format("WizzairHelper {} · ", running.to_string());
    if (!last_success) {
        return head + "never checked";
    }
    const LocalDateTime when = clock.to_local(*last_success);
    const Date day = date_of(when);
    const Date today = date_of(clock.to_local(clock.utc_now()));
    const std::string time = locale.format_time(when, "HH:mm");
    if (day == today) {
        return std::format("{}last checked today at {}", head, time);
    }
    if (day == today.add_days(-1)) {
        return std::format("{}last checked yesterday at {}", head, time);
    }
    return std::format("{}last checked on {} at {}", head, locale.format_date(day, "ddd dd MMM yyyy"), time);
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

bool UpdateModel::set_options(const update::UpdateOptions& options) noexcept
{
    options_ = options;
    return downloads_on_its_own() && options_.check_automatically && newer_pending() && !download_tried_ &&
           !downloading_ && !install_requested_;
}

update::CheckPolicy UpdateModel::check_policy() const
{
    return update::CheckPolicy{.automatic = options_.check_automatically,
                               .frequency = options_.check_frequency,
                               .recheck_if_newer_than = session_checked_ ? std::nullopt
                                                                         : std::optional<update::Version>{running_}};
}

UpdateModel::CheckOutcome UpdateModel::on_check(const CheckResult& result, CheckOrigin origin)
{
    CheckOutcome outcome;
    if (origin == CheckOrigin::manual) {
        checking_ = false;
    }
    if (!result) {
        if (result.error().kind == update::UpdateErrorKind::no_release) {
            session_checked_ = true;  // nothing is published to update to
            up_to_date_ = true;
            problem_.reset();
            return outcome;
        }
        outcome.log.push_back(update_strings::check_failed_log(result.error().message));
        problem_ = update_strings::check_failed_status(result.error());
        return outcome;
    }
    session_checked_ = true;
    problem_.reset();
    const update::ReleaseInfo& release = *result;
    if (!release.newer || release.version <= running_) {
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
    outcome.download = downloads_on_its_own();
    return outcome;
}

void UpdateModel::begin_download() noexcept
{
    downloading_ = true;
    download_tried_ = true;
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
    checking_ = false;
    downloading_ = false;
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
    if (newer_pending() && options_.when_found == update::WhenFound::notify_only) {
        return Offer::update_now;
    }
    return ready_ ? Offer::ready : Offer::none;
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
        const bool on_close = options_.when_found == update::WhenFound::install_on_close && !restart_;
        return UpdateBarView{.visible = true,
                             .text = on_close ? update_strings::ready_text(ready_->version)
                                              : update_strings::ready_to_install_text(ready_->version),
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
                             .text = update_strings::available_text(pending_->version),
                             .action = std::string{update_strings::kUpdateNow},
                             .action_enabled = !working};
    case Offer::downloading_for_install:
        return UpdateBarView{.visible = true,
                             .text = update_strings::downloading_text(pending_->version),
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
    if (!ready_ || kind_ != update::InstallKind::installed) {
        return std::nullopt;
    }
    if (restart_) {
        return update::silent_install_command(ready_->path, update::Relaunch::yes);
    }
    if (options_.when_found == update::WhenFound::install_on_close) {
        return update::silent_install_command(ready_->path, update::Relaunch::no);
    }
    return std::nullopt;
}

UpdateStatusView UpdateModel::status_view(const Clock& clock, const Locale& locale) const
{
    UpdateStatusView view;
    view.version_line = update_strings::version_line(running_, saved_.last_success, clock, locale);
    view.checking = checking_;
    view.check_now_enabled = !checking_ && !downloading_;
    view.options_enabled = true;
    view.when_found_enabled = kind_ == update::InstallKind::installed;
    view.when_found_note = kind_ == update::InstallKind::portable ? std::string{update_strings::kPortableNote} : "";

    const auto set = [&view](UpdateStatusKind kind, std::string text) {
        view.kind = kind;
        view.status = std::move(text);
    };
    const bool notify_offers_newer = options_.when_found == update::WhenFound::notify_only && newer_pending();
    if (checking_) {
        set(UpdateStatusKind::checking, std::string{update_strings::kChecking});
    } else if (downloading_ && pending_) {
        set(UpdateStatusKind::downloading, update_strings::downloading_text(pending_->version));
    } else if (ready_ && !notify_offers_newer) {
        set(UpdateStatusKind::ready, update_strings::ready_to_install_text(ready_->version));
    } else if (problem_) {
        set(UpdateStatusKind::failed, *problem_);
    } else if (pending_ || available_) {
        set(UpdateStatusKind::available,
            update_strings::available_text(pending_ ? pending_->version : available_->version));
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
