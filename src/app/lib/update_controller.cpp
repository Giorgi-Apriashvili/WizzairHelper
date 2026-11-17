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
#include <optional>
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
    hooks.locale = services.locale;
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
    if (!hooks_.locale) {
        hooks_.locale = Locale::en_us();
    }
    model_.set_saved_state(hooks_.backend->load_state());  // a small file: the version line from the start
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
    start_work(Work::automatic);  // the first evaluation (with the reminder) runs, also after work started earlier
}

ui::UpdateStatusView UpdateController::status_view() const
{
    return model_.status_view(hooks_.backend->clock(), *hooks_.locale);
}

void UpdateController::evaluate()
{
    if (!started_ || running_ || closing_) {
        return;  // a tick during running work is skipped
    }
    flows_->spawn(run(Work::automatic));
}

void UpdateController::start_work(Work work)
{
    if (closing_) {
        return;
    }
    if (!running_) {
        flows_->spawn(run(work));
        return;
    }
    switch (work) {  // runs right after the current work (run's loop)
    case Work::manual:
        manual_queued_ = true;
        break;
    case Work::fetch:
        fetch_queued_ = true;
        break;
    case Work::download:
        download_queued_ = true;
        break;
    case Work::automatic:
        automatic_queued_ = true;
        break;
    }
}

Task<> UpdateController::run(Work first)
{
    {
        const RunningGuard guard{running_};
        std::optional<Work> work = first;
        while (work && !closing_) {
            try {
                switch (*work) {
                case Work::automatic:
                    if (!std::exchange(reminded_, true)) {
                        co_await remind();
                    }
                    if (model_.download_retry_due()) {
                        co_await download();
                    } else {
                        co_await automatic_check();
                    }
                    break;
                case Work::manual:
                    co_await check_now_work(ui::CheckOrigin::manual);
                    break;
                case Work::fetch:
                    if (model_.fetch_wanted()) {  // not made unnecessary while it waited
                        co_await check_now_work(ui::CheckOrigin::update_now);
                    }
                    break;
                case Work::download:
                    if (model_.download_wanted()) {  // not withdrawn while it waited (D-61)
                        co_await download();
                    }
                    break;
                }
            } catch (const OperationCancelled&) {
                // The window is closing.
                automatic_checking_ = false;
            } catch (const std::exception& error) {
                // The updater never shows a crash box: one Log line, and nothing stays busy.
                log_(ui::update_strings::check_failed_log(error.what()));
                model_.on_unexpected_error(error.what(), std::exchange(automatic_checking_, false));
                refresh();
            }
            work.reset();
            if (std::exchange(manual_queued_, false)) {
                work = Work::manual;
            } else if (std::exchange(fetch_queued_, false)) {
                work = Work::fetch;
            } else if (std::exchange(download_queued_, false)) {
                work = Work::download;
            } else if (std::exchange(automatic_queued_, false) && started_) {
                work = Work::automatic;
            }
        }
    }
    if (!closing_) {
        Q_EMIT evaluated();
    }
}

Task<> UpdateController::remind()
{
    struct Answer {
        update::UpdateState state;
        std::optional<ui::EarlierUpdate> earlier;  // update-state.json saw a newer version
    };
    // Files only: update-state.json and the downloads folder. Nothing goes to GitHub.
    Answer answer =
        co_await jobs_->run(jobs_->new_job_id(), stop_.get_token(), [backend = hooks_.backend](const JobChannel&) {
            backend->remove_stale_downloads();  // versions up to the running one
            Answer result{.state = backend->load_state(), .earlier = std::nullopt};
            if (const std::optional<update::Version> seen = result.state.latest_seen;
                seen && *seen > backend->running_version()) {
                const bool installed = backend->install_kind() == update::InstallKind::installed;
                result.earlier = ui::EarlierUpdate{.version = *seen,
                                                   .page_url = backend->release_page_url(*seen),
                                                   .installer = installed ? backend->verified_installer(*seen)
                                                                          : std::nullopt};
            }
            return result;
        });
    model_.set_saved_state(answer.state);
    if (answer.earlier) {
        log_lines(model_.remind(*answer.earlier));
    }
    refresh();
}

Task<> UpdateController::automatic_check()
{
    struct Answer {
        std::optional<UpdateCheckResult> result;  // nullopt: no check was due
        update::UpdateState state;
    };
    automatic_checking_ = true;
    Answer answer = co_await jobs_->run(
        jobs_->new_job_id(), stop_.get_token(),
        [backend = hooks_.backend, policy = model_.check_policy()](const JobChannel& channel) {
            std::optional<UpdateCheckResult> result = backend->check_if_due(policy, channel.stop_token());
            return Answer{.result = std::move(result), .state = backend->load_state()};
        });
    automatic_checking_ = false;
    model_.set_saved_state(answer.state);
    if (answer.result) {
        co_await apply_check(std::move(*answer.result), ui::CheckOrigin::automatic);
    }
}

Task<> UpdateController::check_now_work(ui::CheckOrigin origin)
{
    struct Answer {
        UpdateCheckResult result;
        update::UpdateState state;
    };
    if (origin == ui::CheckOrigin::update_now) {
        model_.begin_fetch();
        refresh();  // "Downloading WizzairHelper X…"; Check now waits
    }
    Answer answer = co_await jobs_->run(jobs_->new_job_id(), stop_.get_token(),
                                        [backend = hooks_.backend](const JobChannel& channel) {
                                            UpdateCheckResult result = backend->check_now(channel.stop_token());
                                            return Answer{.result = std::move(result), .state = backend->load_state()};
                                        });
    model_.set_saved_state(answer.state);
    co_await apply_check(std::move(answer.result), origin);
}

Task<> UpdateController::apply_check(ui::CheckResult result, ui::CheckOrigin origin)
{
    const ui::UpdateModel::CheckOutcome outcome = model_.on_check(result, origin);
    log_lines(outcome.log);
    refresh();
    if (outcome.download) {
        co_await download();
    }
}

Task<> UpdateController::download()
{
    if (!model_.pending_download() || model_.downloading()) {
        co_return;  // nothing to download (verified meanwhile), or already running
    }
    const update::ReleaseInfo release = *model_.pending_download();
    model_.begin_download();
    refresh();
    const UpdateDownloadResult file =
        co_await jobs_->run(jobs_->new_job_id(), stop_.get_token(),
                            [backend = hooks_.backend, release](const JobChannel& channel) {
                                return backend->download_installer(release, channel.stop_token());
                            });
    const ui::UpdateModel::DownloadOutcome outcome = model_.on_download(file, working_);
    log_lines(outcome.log);
    refresh();
    if (outcome.restart) {
        Q_EMIT restart_requested();  // "Update now": the window closes, saves its settings, then installs
    }
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
    switch (model_.on_action(working_)) {
    case ui::BarAction::restart:
        refresh();
        Q_EMIT restart_requested();  // the window closes, saves its settings, then on_window_closing
        return;
    case ui::BarAction::open_release_page:
        if (const std::optional<ui::AvailableRelease>& available = model_.available();
            available && hooks_.open_release_page) {
            hooks_.open_release_page(available->page_url);
        }
        return;
    case ui::BarAction::download:
        refresh();  // "Downloading WizzairHelper X…"
        start_work(Work::download);
        return;
    case ui::BarAction::check_then_download:
        refresh();  // "Downloading WizzairHelper X…"
        start_work(Work::fetch);  // the user asked: its release is checked for, then downloaded
        return;
    case ui::BarAction::none:
        refresh();
        return;
    }
}

void UpdateController::dismiss()
{
    model_.dismiss();
    refresh();
}

void UpdateController::set_options(const update::UpdateOptions& options)
{
    const update::UpdateOptions before = model_.options();
    const bool download_now = model_.set_options(options);
    refresh();
    if (closing_) {
        return;
    }
    if (download_now) {
        start_work(Work::download);
    }
    const bool schedule_changed = options.check_automatically != before.check_automatically ||
                                  options.check_frequency != before.check_frequency;
    if (schedule_changed && options.check_automatically && started_) {
        start_work(Work::automatic);  // a check may have become due
    }
}

void UpdateController::check_now()
{
    if (closing_ || model_.checking()) {
        return;
    }
    model_.begin_manual_check();
    Q_EMIT status_changed();
    start_work(Work::manual);
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

void UpdateController::refresh()
{
    refresh_bar();
    Q_EMIT status_changed();
}

} // namespace wizz::app
