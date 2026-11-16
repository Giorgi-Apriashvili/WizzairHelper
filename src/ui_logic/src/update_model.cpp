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

std::string available_text(const update::Version& version)
{
    return std::format("WizzairHelper {} is available.", version.to_string());
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

} // namespace update_strings

UpdateModel::UpdateModel(update::Version running, update::InstallKind kind) noexcept : running_(running), kind_(kind) {}

UpdateModel::CheckOutcome UpdateModel::on_check(const std::expected<update::ReleaseInfo, update::UpdateError>& result)
{
    CheckOutcome outcome;
    if (!result) {
        if (result.error().kind != update::UpdateErrorKind::no_release) {
            outcome.log.push_back(update_strings::check_failed_log(result.error().message));
        }
        return outcome;  // no_release: nothing is published to update to
    }
    const update::ReleaseInfo& release = *result;
    if (!release.newer || release.version <= running_) {
        return outcome;
    }
    // Nothing new when this version (or a newer one) is already ready or offered.
    if ((ready_ && release.version <= ready_->version) || (available_ && release.version <= available_->version) ||
        (pending_ && release.version < pending_->version)) {
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
    pending_ = release;
    retry_download_ = false;
    outcome.download = true;
    return outcome;
}

std::vector<std::string> UpdateModel::on_download(const std::expected<update::DownloadedFile, update::UpdateError>& result)
{
    std::vector<std::string> log;
    if (!pending_) {
        return log;
    }
    if (!result) {
        log.push_back(update_strings::download_failed_log(result.error().message));
        retry_download_ = may_heal(result.error().kind);
        return log;
    }
    ready_ = ReadyInstaller{.version = pending_->version, .path = result->path};
    log.push_back(update_strings::verified_log(pending_->version));
    pending_.reset();
    retry_download_ = false;
    return log;
}

bool UpdateModel::request_restart(bool working) noexcept
{
    if (working || !ready_) {
        return false;
    }
    restart_ = true;
    return true;
}

UpdateBarView UpdateModel::bar(bool working) const
{
    if (dismissed_) {
        return {};
    }
    if (ready_) {
        return UpdateBarView{.visible = true,
                             .text = update_strings::ready_text(ready_->version),
                             .action = std::string{update_strings::kRestartAndUpdate},
                             .action_enabled = !working};
    }
    if (available_) {
        return UpdateBarView{.visible = true,
                             .text = update_strings::available_text(available_->version),
                             .action = std::string{update_strings::kDownload},
                             .action_enabled = true};
    }
    return {};
}

std::optional<update::InstallerCommand> UpdateModel::exit_command() const
{
    if (!ready_) {
        return std::nullopt;
    }
    return update::silent_install_command(ready_->path, restart_ ? update::Relaunch::yes : update::Relaunch::no);
}

} // namespace wizz::ui
