#pragma once
// The self-updater's lifecycle in the app (D-45..D-49), on top of the core
// (wizz/services/updater.hpp) and its rules (wizz/ui/update_model.hpp):
//
//   * start() — after the main window is shown — re-evaluates at once and then every
//     hour, so a window left open for days still checks once a day. Each evaluation
//     runs on a JobRunner worker: the core's check_if_due (no request when no check is
//     due; the 24 h / 1 h schedule is the core's), or, after a download failure that
//     may heal, the download again. Only one evaluation runs at a time; a tick during
//     one is skipped. The first one also deletes downloads of old versions.
//   * Checking is silent: a failure is one Log tab line (the core writes update.log);
//     no boxes, no focus changes.
//   * A newer release: an installed copy downloads and verifies the installer in the
//     background, then the bar offers "Restart and update"; a portable copy's bar
//     links to the release page ("Download" opens it in the default browser).
//   * "Later" hides the bar for the session; the update still installs on exit.
//   * on_window_closing() (MainWindow::closeEvent, after the settings were saved)
//     stops any running check or download and, when a verified installer is ready,
//     releases the app mutex and starts the installer detached with the silent
//     command line — with /RELAUNCH after "Restart and update" — then the app exits.
//     A failed start is logged (Log tab and update.log) and the app exits normally.
//     Nothing is started while Windows ends the session.
//   * "Restart and update" (disabled while a search or phase is working) asks the
//     window to close (restart_requested): closing saves the settings, then the
//     installer starts with /RELAUNCH.
//
// The controller exists only in normal runs: MainWindow::enable_updates refuses
// --autotest runs, and main never enables it with --autotest or --replay on the
// command line (ui::updates_allowed). UI thread only.

#include "installer_launcher.hpp"
#include "job_runner.hpp"
#include "task.hpp"
#include "task_scope.hpp"
#include "update_backend.hpp"

#include "wizz/ui/update_model.hpp"

#include <QObject>

#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

class QTimer;

namespace wizz::app {

struct AppServices;
class UpdateBar;

/// How often the controller re-evaluates whether a check is due.
inline constexpr std::chrono::milliseconds kUpdateRecheckInterval = std::chrono::hours{1};

/// What the controller needs from the outside world.
struct UpdateHooks {
    std::shared_ptr<UpdateBackend> backend;
    /// Starts the installer (launch_detached in the app).
    InstallerLauncher launch_installer;
    /// Runs right before the installer starts: releases the AppMutex (main).
    std::function<void()> release_app_mutex;
    /// Opens a portable copy's release page ("Download").
    std::function<void(const std::string& url)> open_release_page;
    std::chrono::milliseconds recheck_interval = kUpdateRecheckInterval;

    /// The hooks of a normal run: CoreUpdateBackend for WIZZ_VERSION_STRING in
    /// `<data root>\updates`, sharing the services' transport and clock, the install kind
    /// of the running exe, launch_detached, and the default browser (a Warning box
    /// when it cannot be opened, as the grids do).
    [[nodiscard]] static UpdateHooks system(const AppServices& services, std::function<void()> release_app_mutex);
};

class UpdateController final : public QObject {
    Q_OBJECT

public:
    using LogLine = std::function<void(std::string_view line)>;

    /// `bar` must outlive the controller; `log` appends a Log tab line. Throws
    /// std::invalid_argument without a backend.
    UpdateController(UpdateHooks hooks, UpdateBar& bar, LogLine log, QObject* parent = nullptr);
    /// Stops and joins a running check or download.
    ~UpdateController() override;

    UpdateController(const UpdateController&) = delete;
    UpdateController& operator=(const UpdateController&) = delete;

    /// After the window is shown: evaluates now and starts the hourly timer. Once only.
    void start();
    /// The window is closing (settings already saved): stops the timer and any running
    /// work, then starts a ready installer (see above). Once only.
    void on_window_closing();

    [[nodiscard]] const ui::UpdateModel& model() const noexcept { return model_; }
    [[nodiscard]] QTimer* recheck_timer() const noexcept { return timer_; }
    [[nodiscard]] bool is_started() const noexcept { return started_; }
    /// A check or download is running on a worker.
    [[nodiscard]] bool is_running() const noexcept { return running_; }
    [[nodiscard]] bool is_closing() const noexcept { return closing_; }

public Q_SLOTS:
    /// The hourly tick: one evaluation unless one is running or the window is closing.
    void evaluate();
    /// Controller::working_changed: "Restart and update" waits while work runs.
    void set_working(bool working);
    /// The bar's action: "Restart and update" or "Download".
    void trigger_action();
    /// "Later".
    void dismiss();

Q_SIGNALS:
    /// "Restart and update" was accepted: the window should close.
    void restart_requested();
    /// An evaluation finished (the model and the bar are up to date).
    void evaluated();

private:
    [[nodiscard]] Task<> run_evaluation();
    [[nodiscard]] Task<> download();
    void log_lines(const std::vector<std::string>& lines);
    void refresh_bar();

    UpdateHooks hooks_;
    UpdateBar& bar_;
    LogLine log_;
    ui::UpdateModel model_;
    QTimer* timer_ = nullptr;
    bool started_ = false;
    bool running_ = false;
    bool closing_ = false;
    bool working_ = false;
    bool cleaned_up_ = false;
    std::stop_source stop_;
    std::unique_ptr<JobRunner> jobs_;
    std::unique_ptr<TaskScope> flows_;
};

} // namespace wizz::app
