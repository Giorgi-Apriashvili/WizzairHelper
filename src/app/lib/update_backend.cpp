#include "update_backend.hpp"

#include <stdexcept>
#include <utility>

namespace wizz::app {

namespace {

[[nodiscard]] std::shared_ptr<const Clock> checked_clock(std::shared_ptr<const Clock> clock)
{
    if (!clock) {
        throw std::invalid_argument("CoreUpdateBackend needs a clock");
    }
    return clock;
}

} // namespace

CoreUpdateBackend::CoreUpdateBackend(update::UpdateConfig config, update::InstallKind kind,
                                     std::shared_ptr<http::Transport> transport, std::shared_ptr<const Clock> clock,
                                     DelayFunc delay)
    : kind_(kind),
      clock_(checked_clock(std::move(clock))),
      updater_(config, std::move(transport), clock_, std::move(delay)),
      schedule_(config.updates_dir(), clock_)
{
}

update::Version CoreUpdateBackend::running_version() const
{
    return updater_.config().current_version;
}

std::optional<UpdateCheckResult> CoreUpdateBackend::check_if_due(const update::CheckPolicy& policy,
                                                                 std::stop_token stop)
{
    return updater_.check_if_due(schedule_, policy, std::move(stop));
}

UpdateCheckResult CoreUpdateBackend::check_now(std::stop_token stop)
{
    return updater_.check_now(schedule_, std::move(stop));
}

update::UpdateState CoreUpdateBackend::load_state() const
{
    return schedule_.load();
}

std::optional<std::filesystem::path> CoreUpdateBackend::verified_installer(const update::Version& version) const
{
    return update::find_verified_download(updater_.config(), version, update::AssetKind::installer);
}

std::string CoreUpdateBackend::release_page_url(const update::Version& version) const
{
    return updater_.config().release_page_url(version);
}

UpdateDownloadResult CoreUpdateBackend::download_installer(const update::ReleaseInfo& release, std::stop_token stop)
{
    return updater_.download_verified(release, update::AssetKind::installer, updater_.config().download_dir(),
                                      std::move(stop));
}

void CoreUpdateBackend::remove_stale_downloads()
{
    (void)update::remove_stale_downloads(updater_.config());
}

void CoreUpdateBackend::log(std::string_view text)
{
    update::append_log_line(updater_.config().log_file(), clock_->utc_now(), text);
}

} // namespace wizz::app
