#include "update_controller.hpp"

#include "app_services.hpp"
#include "grid_actions.hpp"
#include "qt_strings.hpp"
#include "update_bar.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/services/app_paths.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QTimer>

#include <exception>
#include <format>
#include <stdexcept>
#include <utility>

namespace wizz::app {

namespace {

/// Clears the running flag when an evaluation ends, also when its frame is destroyed
/// with the controller.
class RunningGuard {
public:
    explicit RunningGuard(bool& running) noexcept : running_(running) { running_ = true; }
    RunningGuard(const RunningGuard&) = delete;
    RunningGuard& operator=(const RunningGuard&) = delete;
    ~RunningGuard() { running_ = false; }

private:
    bool& running_;
};

[[nodiscard]] UpdateHooks checked(UpdateHooks hooks)
{
    if (!hooks.backend) {
        throw std::invalid_argument("UpdateController needs an update backend");
    }
    return hooks;
}

/// Windows is logging off or shutting down (Qt's commit-data request is running).
[[nodiscard]] bool session_is_ending()
{
#if QT_CONFIG(sessionmanager)
    const auto* application = qobject_cast<const QGuiApplication*>(QCoreApplication::instance());
    return application != nullptr && application->isSavingSession();
#else
    return false;
#endif
}

} // namespace

UpdateHooks UpdateHooks::system(const AppServices& services, std::function<void()> release_app_mutex)
{
    update::UpdateConfig config =
        update::UpdateConfig::for_app(WIZZ_VERSION_STRING, app_paths::default_root(services.data_root));
    const update::InstallKind kind = update::detect_install_kind(update::running_executable_path());
    UpdateHooks hooks;
    hooks.backend = std::make_shared<CoreUpdateBackend>(std::move(config), kind, services.transport, services.clock);
    hooks.launch_installer = launch_detached;
    hooks.release_app_mutex = std::move(release_app_mutex);
    hooks.open_release_page = [](const std::string& url) {
        (void)open_url_or_warn(GridActions::system(), url);  // a Warning box names the URL on failure
    };
    return hooks;
}

UpdateController::UpdateController(UpdateHooks hooks, UpdateBar& bar, LogLine log, QObject* parent)
    : QObject(parent),
      hooks_(checked(std::move(hooks))),
      bar_(bar),
      log_(std::move(log)),
      model_(hooks_.backend->running_version(), hooks_.backend->install_kind()),
      timer_(new QTimer(this)),
      jobs_(std::make_unique<JobRunner>()),
      flows_(std::make_unique<TaskScope>())
{
    timer_->setInterval(hooks_.recheck_interval);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &UpdateController::evaluate);
}

UpdateController::~UpdateController()
{
    closing_ = true;
    stop_.request_stop();
    flows_.reset();  // a suspended evaluation is destroyed
    jobs_.reset();   // stops and joins its worker
}

void UpdateController::start()
{
    if (started_ || closing_) {
        return;
    }
    started_ = true;
    timer_->start();
    evaluate();
}

void UpdateController::evaluate()
{
    if (!started_ || running_ || closing_) {
        return;  // a tick during a running check or download is skipped
    }
    flows_->spawn(run_evaluation());
}

Task<> UpdateController::run_evaluation()
{
    {
        const RunningGuard guard{running_};
        try {
            if (model_.download_retry_due()) {
                co_await download();
            } else {
                const bool clean_up = !std::exchange(cleaned_up_, true);
                const std::optional<UpdateCheckResult> result = co_await jobs_->run(
                    jobs_->new_job_id(), stop_.get_token(),
                    [backend = hooks_.backend, clean_up](const JobChannel& channel) {
                        if (clean_up) {
                            backend->remove_stale_downloads();
                        }
                        return backend->check_if_due(channel.stop_token());
                    });
                if (result) {  // nullopt: no check was due
                    const ui::UpdateModel::CheckOutcome outcome = model_.on_check(*result);
                    log_lines(outcome.log);
                    refresh_bar();
                    if (outcome.download) {
                        co_await download();
                    }
                }
            }
        } catch (const OperationCancelled&) {
            // The window is closing.
        } catch (const std::exception& error) {
            log_(ui::update_strings::check_failed_log(error.what()));  // the updater never shows a crash box
        }
    }
    if (!closing_) {
        Q_EMIT evaluated();
    }
}

Task<> UpdateController::download()
{
    const update::ReleaseInfo release = *model_.pending_download();
    const UpdateDownloadResult file =
        co_await jobs_->run(jobs_->new_job_id(), stop_.get_token(),
                            [backend = hooks_.backend, release](const JobChannel& channel) {
                                return backend->download_installer(release, channel.stop_token());
                            });
    log_lines(model_.on_download(file));
    refresh_bar();
}

void UpdateController::set_working(bool working)
{
    working_ = working;
    refresh_bar();
}

void UpdateController::trigger_action()
{
    if (closing_) {
        return;
    }
    if (model_.ready()) {
        if (model_.request_restart(working_)) {
            Q_EMIT restart_requested();  // the window closes, saves its settings, then on_window_closing
        }
        return;
    }
    if (const std::optional<ui::AvailableRelease>& available = model_.available();
        available && hooks_.open_release_page) {
        hooks_.open_release_page(available->page_url);
    }
}

void UpdateController::dismiss()
{
    model_.dismiss();
    refresh_bar();
}

void UpdateController::on_window_closing()
{
    if (closing_) {
        return;
    }
    closing_ = true;
    timer_->stop();
    stop_.request_stop();  // a running check or download ends; its worker is joined with the controller
    if (session_is_ending()) {
        return;  // never start an installer while Windows logs off or shuts down
    }
    const std::optional<update::InstallerCommand> command = model_.exit_command();
    if (!command) {
        return;
    }
    const update::Version version = model_.ready()->version;
    const std::string text = to_utf8(QString::fromStdWString(command->command_line()));
    if (hooks_.release_app_mutex) {
        hooks_.release_app_mutex();  // the installer's AppMutex check must not find this copy (D-47)
    }
    const std::expected<void, std::string> started =
        hooks_.launch_installer ? hooks_.launch_installer(*command)
                                : std::expected<void, std::string>{std::unexpect, "no installer launcher"};
    if (started) {
        log_(ui::update_strings::install_started_log(version));
        hooks_.backend->log(std::format("Started {}", text));
    } else {
        log_(ui::update_strings::install_failed_log(version, started.error()));
        hooks_.backend->log(std::format("Could not start {}: {}", text, started.error()));
    }
}

void UpdateController::log_lines(const std::vector<std::string>& lines)
{
    for (const std::string& line : lines) {
        log_(line);
    }
}

void UpdateController::refresh_bar()
{
    bar_.show_view(model_.bar(working_));
}

} // namespace wizz::app
