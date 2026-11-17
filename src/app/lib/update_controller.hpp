#pragma once
// The self-updater's lifecycle in the app (D-45..D-49, D-59..D-62), on top of the core
// (wizz/services/updater.hpp) and its rules (wizz/ui/update_model.hpp):
//
//   * start() — after the main window is shown — re-evaluates at once and then every
//     hour, so a window left open across a day, week or month boundary checks at the next
//     tick. Each evaluation runs on a JobRunner worker: the core's check_if_due with the
//     model's policy (no request when no check is due or automatic checks are off; the
//     calendar periods and the 1 h failure floor are the core's), or, after a download
//     failure that may heal, the download again. The first one first deletes downloads
//     of old versions and offers again an update put off in an earlier session, from
//     update-state.json and the downloads folder alone — no request (the model's remind).
//   * One piece of work at a time: an automatic evaluation, a manual check (check_now,
//     the Settings window's Check now), the check of "Update now" for a remembered update,
//     or a download (after a check, "Update now", or a switch to a choice that downloads).
//     Work asked for while another runs is queued and runs right after it (manual check,
//     then "Update now"'s check, then download, then evaluation); an hourly tick during
//     work is skipped. A queued download or "Update now" check starts only if the model
//     still wants it (the choice or the switch changed back, "Later", or another check
//     answered meanwhile: it is dropped).
//   * Checking is silent: a failure is one Log tab line (the core writes update.log);
//     no boxes, no focus changes. The Settings window reads status_view() and follows
//     status_changed().
//   * A newer release: what happens is the model's (the "When an update is found"
//     choice; a portable copy always links to the release page).
//   * set_options() — the Settings window — applies new options at once: a check that
//     became due runs now, a choice that downloads starts the download of an offered
//     update. Saving settings.json is the caller's job.
//   * "Later" hides the bar for the session.
//   * on_window_closing() (MainWindow::closeEvent, after the settings were saved)
//     stops any running check or download and, when the model's exit_command() says so
//     (a verified installer with "install when I close the app", or after "Restart and
//     update" / a completed "Update now"), releases the app mutex and starts the
//     installer detached with the silent command line — with /RELAUNCH after a restart
//     request — then the app exits. A failed start is logged (Log tab and update.log)
//     and the app exits normally. Nothing is started while Windows ends the session.
//   * "Restart and update" (disabled while a search or phase is working) asks the
//     window to close (restart_requested): closing saves the settings, then the
//     installer starts with /RELAUNCH. A completed "Update now" does the same unless a
//     search or phase is working.
//
// The controller exists only in normal runs: MainWindow::enable_updates refuses
// --autotest runs, and main never enables it with --autotest or --replay on the
// command line (ui::updates_allowed); the Settings window then shows
// ui::updates_off_view(). UI thread only.

#include "installer_launcher.hpp"
#include "job_runner.hpp"
#include "task.hpp"
#include "task_scope.hpp"
#include "update_backend.hpp"

#include "wizz/core/locale.hpp"
#include "wizz/services/update_options.hpp"
#include "wizz/ui/update_model.hpp"

#include <QObject>

#include <chrono>
#include <cstddef>
#include <cstdint>
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
    /// Formats the date of the Settings window's "last checked" line; en-US when null.
    std::shared_ptr<const Locale> locale;

    /// The hooks of a normal run: CoreUpdateBackend for WIZZ_VERSION_STRING in
    /// `<data root>\updates`, sharing the services' transport and clock, the install kind
    /// of the running exe, launch_detached, the default browser (a Warning box when it
    /// cannot be opened, as the grids do) and the services' locale.
    [[nodiscard]] static UpdateHooks system(const AppServices& services, std::function<void()> release_app_mutex);
};

class UpdateController final : public QObject {
    Q_OBJECT

public:
    using LogLine = std::function<void(std::string_view line)>;

    /// `bar` must outlive the controller; `log` appends a Log tab line. Reads
    /// update-state.json once for the Settings window's version line. Throws
    /// std::invalid_argument without a backend.
    UpdateController(UpdateHooks hooks, UpdateBar& bar, LogLine log, QObject* parent = nullptr);
    /// Stops and joins a running check or download.
    ~UpdateController() override;

    UpdateController(const UpdateController&) = delete;
    UpdateController& operator=(const UpdateController&) = delete;

    /// After the window is shown: evaluates now and starts the hourly timer. Once only.
    void start();
    /// The window is closing (settings already saved): stops the timer and any running
    /// work, then starts the installer when the model's exit_command() says so. Once only.
    void on_window_closing();

    [[nodiscard]] const ui::UpdateModel& model() const noexcept { return model_; }
    /// The Settings window's "Updates" section, now.
    [[nodiscard]] ui::UpdateStatusView status_view() const;
    [[nodiscard]] const update::UpdateOptions& options() const noexcept { return model_.options(); }
    [[nodiscard]] QTimer* recheck_timer() const noexcept { return timer_; }
    [[nodiscard]] bool is_started() const noexcept { return started_; }
    /// A check or download is running on a worker.
    [[nodiscard]] bool is_running() const noexcept { return running_; }
    /// Checks and downloads started on workers and not yet completed: never more than one.
    [[nodiscard]] std::size_t live_jobs() const noexcept { return jobs_->live_count(); }
    [[nodiscard]] bool is_closing() const noexcept { return closing_; }

public Q_SLOTS:
    /// The hourly tick: one evaluation unless work is running or the window is closing.
    void evaluate();
    /// Controller::working_changed: "Restart and update" and "Update now" wait while work runs.
    void set_working(bool working);
    /// The bar's action: "Restart and update", "Update now" (for a remembered update: a
    /// check for its release first, then the download) or "Download".
    void trigger_action();
    /// "Later".
    void dismiss();
    /// The Settings window changed the options (see the file comment).
    void set_options(const wizz::update::UpdateOptions& options);
    /// The Settings window's Check now: one check right away (after running work), even
    /// when automatic checks are off; its result shows in status_view().
    void check_now();

Q_SIGNALS:
    /// "Restart and update" was accepted, or "Update now" completed: the window should close.
    void restart_requested();
    /// A run of work finished — an evaluation, a manual check or a download, with the work
    /// queued behind it (the model and the bar are up to date).
    void evaluated();
    /// status_view() may have changed: the Settings window re-reads it.
    void status_changed();

private:
    enum class Work : std::uint8_t { automatic, manual, fetch, download };

    /// Runs `work` now, or after the running work.
    void start_work(Work work);
    [[nodiscard]] Task<> run(Work first);
    /// The first evaluation: old downloads go, an earlier session's update is offered again.
    [[nodiscard]] Task<> remind();
    [[nodiscard]] Task<> automatic_check();
    /// A check that always runs: Check now (manual) or "Update now"'s (update_now).
    [[nodiscard]] Task<> check_now_work(ui::CheckOrigin origin);
    [[nodiscard]] Task<> apply_check(ui::CheckResult result, ui::CheckOrigin origin);
    [[nodiscard]] Task<> download();
    void log_lines(const std::vector<std::string>& lines);
    void refresh_bar();
    /// refresh_bar() and status_changed().
    void refresh();

    UpdateHooks hooks_;
    UpdateBar& bar_;
    LogLine log_;
    ui::UpdateModel model_;
    QTimer* timer_ = nullptr;
    bool started_ = false;
    bool running_ = false;
    bool closing_ = false;
    bool working_ = false;
    bool reminded_ = false;
    /// automatic_check()'s worker runs: an exception now ended a check (D-45, D-62).
    bool automatic_checking_ = false;
    bool manual_queued_ = false;
    bool fetch_queued_ = false;
    bool download_queued_ = false;
    bool automatic_queued_ = false;
    std::stop_source stop_;
    std::unique_ptr<JobRunner> jobs_;
    std::unique_ptr<TaskScope> flows_;
};

} // namespace wizz::app
