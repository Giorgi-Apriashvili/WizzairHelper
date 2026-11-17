#pragma once
// The self-updater core as the update controller uses it (update_controller.hpp,
// D-45): an interface, so widget tests script checks and downloads without a
// network, and CoreUpdateBackend, the real one over wizz::update (D-40..D-44).
//
// check_if_due, check_now, download_installer and remove_stale_downloads block and run
// on a worker thread; the controller never runs two of them at once. load_state,
// verified_installer, release_page_url and log send nothing and may be called from any
// thread.

#include "wizz/core/cancellation.hpp"
#include "wizz/core/http.hpp"
#include "wizz/core/time.hpp"
#include "wizz/services/updater.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace wizz::app {

using UpdateCheckResult = std::expected<update::ReleaseInfo, update::UpdateError>;
using UpdateDownloadResult = std::expected<update::DownloadedFile, update::UpdateError>;

class UpdateBackend {
public:
    UpdateBackend() = default;
    virtual ~UpdateBackend() = default;
    UpdateBackend(const UpdateBackend&) = delete;
    UpdateBackend& operator=(const UpdateBackend&) = delete;

    /// The running version (WIZZ_VERSION_STRING).
    [[nodiscard]] virtual update::Version running_version() const = 0;
    /// Installed (unins000.exe next to the exe) or portable.
    [[nodiscard]] virtual update::InstallKind install_kind() const = 0;
    /// The schedule's clock: its local time is the one of the calendar periods (D-59) and
    /// of the Settings window's "last checked" line.
    [[nodiscard]] virtual const Clock& clock() const = 0;

    /// Worker: the automatic check — nullopt without any request when none is due under
    /// `policy` (D-59), else the releases/latest result, recorded in the schedule. Throws
    /// OperationCancelled.
    [[nodiscard]] virtual std::optional<UpdateCheckResult> check_if_due(const update::CheckPolicy& policy,
                                                                        std::stop_token stop) = 0;
    /// Worker: Check now — always the releases/latest request, recorded in the schedule
    /// (a success is the period's check). Throws OperationCancelled.
    [[nodiscard]] virtual UpdateCheckResult check_now(std::stop_token stop) = 0;
    /// update-state.json as it is now; the empty state when it cannot be read.
    [[nodiscard]] virtual update::UpdateState load_state() const = 0;
    /// The verified installer of `version` in the downloads folder, without any request
    /// (update::find_verified_download); nullopt when there is none.
    [[nodiscard]] virtual std::optional<std::filesystem::path> verified_installer(
        const update::Version& version) const = 0;
    /// The release page of `version` (update::UpdateConfig::release_page_url).
    [[nodiscard]] virtual std::string release_page_url(const update::Version& version) const = 0;
    /// Worker: downloads and verifies `release`'s installer into the downloads folder.
    /// Throws OperationCancelled.
    [[nodiscard]] virtual UpdateDownloadResult download_installer(const update::ReleaseInfo& release,
                                                                  std::stop_token stop) = 0;
    /// Worker: deletes downloads of versions up to the running one.
    virtual void remove_stale_downloads() = 0;
    /// Appends `<now> <text>` to update.log.
    virtual void log(std::string_view text) = 0;
};

/// The real backend: update::Updater and update::UpdateSchedule in
/// `config.updates_dir()`, sharing the app's transport and clock.
class CoreUpdateBackend final : public UpdateBackend {
public:
    /// Throws std::invalid_argument for a null transport or clock.
    CoreUpdateBackend(update::UpdateConfig config, update::InstallKind kind, std::shared_ptr<http::Transport> transport,
                      std::shared_ptr<const Clock> clock, DelayFunc delay = cancellable_sleep);

    [[nodiscard]] update::Version running_version() const override;
    [[nodiscard]] update::InstallKind install_kind() const override { return kind_; }
    [[nodiscard]] const Clock& clock() const override { return *clock_; }
    [[nodiscard]] std::optional<UpdateCheckResult> check_if_due(const update::CheckPolicy& policy,
                                                                std::stop_token stop) override;
    [[nodiscard]] UpdateCheckResult check_now(std::stop_token stop) override;
    [[nodiscard]] update::UpdateState load_state() const override;
    [[nodiscard]] std::optional<std::filesystem::path> verified_installer(
        const update::Version& version) const override;
    [[nodiscard]] std::string release_page_url(const update::Version& version) const override;
    [[nodiscard]] UpdateDownloadResult download_installer(const update::ReleaseInfo& release,
                                                          std::stop_token stop) override;
    void remove_stale_downloads() override;
    void log(std::string_view text) override;

    [[nodiscard]] const update::UpdateConfig& config() const noexcept { return updater_.config(); }

private:
    update::InstallKind kind_;
    std::shared_ptr<const Clock> clock_;
    update::Updater updater_;
    update::UpdateSchedule schedule_;
};

} // namespace wizz::app
