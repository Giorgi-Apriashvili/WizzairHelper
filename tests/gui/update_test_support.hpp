#pragma once
// The self-updater in widget tests (D-45..D-49): a scripted core over the real
// daily schedule and a fixed clock, a launcher that records instead of starting a
// process, and a main window with the updater turned on.

#include "app_test_support.hpp"

#include "update_backend.hpp"
#include "update_bar.hpp"
#include "update_controller.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/services/updater.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace wizz::test::app {

inline const update::Version kRunningVersion{1, 0, 0};

/// A release as releases/latest describes it once parsed (D-41).
[[nodiscard]] inline update::ReleaseInfo release_info(update::Version version, bool newer = true)
{
    const std::string text = version.to_string();
    const auto asset = [](const std::string& name, int id) {
        return update::ReleaseAsset{
            .name = name,
            .browser_download_url =
                "https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/download/v1.2.0/" + name,
            .api_url = "https://api.github.com/repos/Giorgi-Apriashvili/WizzairHelper/releases/assets/" +
                       std::to_string(id),
            .size = 1000};
    };
    return update::ReleaseInfo{.tag = "v" + text,
                               .version = version,
                               .html_url = "https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v" + text,
                               .newer = newer,
                               .installer = asset(update::UpdateConfig::installer_name(version), 101),
                               .portable_zip = asset(update::UpdateConfig::zip_name(version), 102),
                               .checksums = asset("SHA256SUMS.txt", 103)};
}

[[nodiscard]] inline update::UpdateError update_error(update::UpdateErrorKind kind, std::string message)
{
    return update::UpdateError{.kind = kind, .message = std::move(message), .http_status = std::nullopt, .tries = {}};
}

/// The core with scripted answers: check_if_due asks the real UpdateSchedule (in
/// `updates_dir`, on `clock`) and records the scripted result in it, like
/// Updater::check_if_due; downloads answer from their own script. Thread-safe.
class FakeUpdateBackend final : public wizz::app::UpdateBackend {
public:
    FakeUpdateBackend(update::InstallKind kind, std::filesystem::path updates_dir, std::shared_ptr<FixedClock> clock)
        : kind_(kind), downloads_dir_(updates_dir / "downloads"), clock_(clock), schedule_(std::move(updates_dir), clock)
    {
    }

    [[nodiscard]] update::Version running_version() const override { return kRunningVersion; }
    [[nodiscard]] update::InstallKind install_kind() const override { return kind_; }

    [[nodiscard]] std::optional<wizz::app::UpdateCheckResult> check_if_due(std::stop_token stop) override
    {
        throw_if_cancelled(stop);
        const std::scoped_lock lock{mutex_};
        worker_threads_.push_back(std::this_thread::get_id());
        ++evaluations_;
        if (!schedule_.is_check_due()) {
            return std::nullopt;
        }
        ++checks_;
        wizz::app::UpdateCheckResult result = next(check_script_, wizz::app::UpdateCheckResult{std::unexpected(
                                                                      update_error(update::UpdateErrorKind::no_release,
                                                                                   "No release is published yet "
                                                                                   "(GitHub answered HTTP 404)."))});
        schedule_.record(result);
        return result;
    }

    [[nodiscard]] wizz::app::UpdateDownloadResult download_installer(const update::ReleaseInfo& release,
                                                                     std::stop_token stop) override
    {
        throw_if_cancelled(stop);
        const std::scoped_lock lock{mutex_};
        worker_threads_.push_back(std::this_thread::get_id());
        downloaded_.push_back(release.version);
        return next(download_script_,
                    wizz::app::UpdateDownloadResult{update::DownloadedFile{
                        .path = installer_path(release.version), .sha256 = std::string(64, 'a'), .reused = false, .tries = {}}});
    }

    void remove_stale_downloads() override { ++cleanups_; }

    void log(std::string_view text) override
    {
        const std::scoped_lock lock{mutex_};
        log_.emplace_back(text);
    }

    // --- Script (results are used in order; the last one repeats) --------------------
    void script_checks(std::vector<wizz::app::UpdateCheckResult> results)
    {
        const std::scoped_lock lock{mutex_};
        check_script_.assign(results.begin(), results.end());
    }
    void script_downloads(std::vector<wizz::app::UpdateDownloadResult> results)
    {
        const std::scoped_lock lock{mutex_};
        download_script_.assign(results.begin(), results.end());
    }

    // --- Observations ------------------------------------------------------------------
    [[nodiscard]] int evaluations() const { return evaluations_; }
    [[nodiscard]] int checks() const { return checks_; }
    [[nodiscard]] int cleanups() const { return cleanups_; }
    [[nodiscard]] std::vector<update::Version> downloaded() const
    {
        const std::scoped_lock lock{mutex_};
        return downloaded_;
    }
    [[nodiscard]] std::vector<std::thread::id> worker_threads() const
    {
        const std::scoped_lock lock{mutex_};
        return worker_threads_;
    }
    [[nodiscard]] std::vector<std::string> log_lines() const
    {
        const std::scoped_lock lock{mutex_};
        return log_;
    }
    [[nodiscard]] std::filesystem::path installer_path(update::Version version) const
    {
        return downloads_dir_ / update::UpdateConfig::installer_name(version);
    }
    [[nodiscard]] const update::UpdateSchedule& schedule() const noexcept { return schedule_; }
    [[nodiscard]] FixedClock& clock() const noexcept { return *clock_; }

private:
    template <class T>
    [[nodiscard]] static T next(std::deque<T>& script, T fallback)
    {
        if (script.empty()) {
            return fallback;
        }
        T result = script.front();
        if (script.size() > 1) {
            script.pop_front();
        }
        return result;
    }

    update::InstallKind kind_;
    std::filesystem::path downloads_dir_;
    std::shared_ptr<FixedClock> clock_;
    update::UpdateSchedule schedule_;
    mutable std::mutex mutex_;
    std::deque<wizz::app::UpdateCheckResult> check_script_;
    std::deque<wizz::app::UpdateDownloadResult> download_script_;
    std::vector<update::Version> downloaded_;
    std::vector<std::thread::id> worker_threads_;
    std::vector<std::string> log_;
    std::atomic<int> evaluations_{0};
    std::atomic<int> checks_{0};
    std::atomic<int> cleanups_{0};
};

/// Records installer starts, app-mutex releases and opened release pages.
struct LaunchLog {
    std::vector<update::InstallerCommand> commands;
    int mutex_releases = 0;
    /// The app mutex had been released when each launch happened.
    std::vector<bool> released_before_launch;
    std::vector<std::string> opened_pages;
    std::optional<std::string> launch_error;  ///< every launch fails with this text

    [[nodiscard]] wizz::app::UpdateHooks hooks(std::shared_ptr<wizz::app::UpdateBackend> backend,
                                               std::chrono::milliseconds interval = wizz::app::kUpdateRecheckInterval)
    {
        return wizz::app::UpdateHooks{
            .backend = std::move(backend),
            .launch_installer = [this](const update::InstallerCommand& command) -> std::expected<void, std::string> {
                commands.push_back(command);
                released_before_launch.push_back(mutex_releases > 0);
                if (launch_error) {
                    return std::unexpected(*launch_error);
                }
                return {};
            },
            .release_app_mutex = [this] { ++mutex_releases; },
            .open_release_page = [this](const std::string& url) { opened_pages.push_back(url); },
            .recheck_interval = interval,
        };
    }
};

struct UpdateRigOptions {
    update::InstallKind kind = update::InstallKind::installed;
    std::chrono::milliseconds interval = wizz::app::kUpdateRecheckInterval;
    WindowRigOptions window{};
};

/// A main window with the updater on, a FakeUpdateBackend in `<data root>\updates`
/// and a recording launcher. The clock starts at 2026-11-02T10:00:00Z.
class UpdateRig {
public:
    explicit UpdateRig(UpdateRigOptions options = {})
        : window_rig(std::move(options.window)),
          clock(std::make_shared<FixedClock>()),
          backend(std::make_shared<FakeUpdateBackend>(options.kind, window_rig.data_root() / "updates", clock)),
          interval_(options.interval)
    {
    }

    /// MainWindow::enable_updates with the fake backend and the recording hooks; from then
    /// on the rig counts the evaluations the controller completes.
    bool enable()
    {
        const bool enabled = window_rig.window().enable_updates(launches.hooks(backend, interval_));
        if (enabled) {
            QObject::connect(controller(), &wizz::app::UpdateController::evaluated, controller(),
                             [this] { ++completed_; });
        }
        return enabled;
    }

    /// enable(), show the window, wait for the start-up map load and the first evaluation.
    void start()
    {
        ASSERT_TRUE(enable());
        window_rig.start();
        wait_for_evaluations(1);
    }

    /// Waits until the controller has completed `count` evaluations (its `evaluated` signal:
    /// the worker's answer has been applied on the UI thread). A count only grows, so this
    /// needs no moment in which no evaluation runs — with a short recheck interval on a busy
    /// machine the next tick may start one as soon as the last ended.
    void wait_for_evaluations(int count, std::chrono::milliseconds timeout = kWaitCap)
    {
        ASSERT_TRUE(gui::wait_until([&] { return completed_ >= count; }, timeout))
            << "evaluations completed: " << completed_ << ", started on the core: " << backend->evaluations();
        gui::settle();
    }

    [[nodiscard]] wizz::app::MainWindow& window() { return window_rig.window(); }
    [[nodiscard]] wizz::app::UpdateController* controller() { return window_rig.window().update_controller(); }
    [[nodiscard]] wizz::app::UpdateBar& bar() { return *window_rig.window().update_bar(); }

    WindowRig window_rig;
    std::shared_ptr<FixedClock> clock;
    std::shared_ptr<FakeUpdateBackend> backend;
    LaunchLog launches;

private:
    std::chrono::milliseconds interval_;
    int completed_ = 0;
};

} // namespace wizz::test::app
