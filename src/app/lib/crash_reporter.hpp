#pragma once
// Crash reporting (01 §3.2 `Program.ShowCrash`, §18.1):
//   1. append "[" + local now yyyy-MM-ddTHH:mm:ss + "] " + details + CRLF CRLF to
//      <root>\errors.log (root = %LOCALAPPDATA%\WizzairHelper or the --data-root
//      override of D-30), creating the directory; I/O errors are ignored;
//   2. show a MessageBox without owner: caption "WizzairHelper", OK, Error icon,
//      "Something went wrong:\n\n{message}\n\nDetails were written to
//      %LOCALAPPDATA%\WizzairHelper\errors.log." (the literal %LOCALAPPDATA%).
//
// `details` plays the part of .NET's ex.ToString(): the exception type and
// message, nested exceptions (std::nested_exception, as " ---> " inner
// exceptions) and the stack trace from the throw site. MSVC x64 runs a catch
// block only after the throwing frames are unwound, so while installed the
// reporter records the stack of every C++ throw per thread (a vectored exception
// handler: an address capture, symbols are resolved only for a crash report).
// std::rethrow_exception is a throw of its own; exceptions that travel through
// co_await or from a worker job to the UI thread are carried as TracedException,
// which hands the original throw site to the next rethrow, so the record still
// starts where the exception was first thrown (.NET keeps those frames across await).
//
// Hooks (01 §3.2 "C++"): Application::notify (application.hpp) routes
// exceptions that escape UI-thread event handlers and posted tasks through
// run_guarded / report_current_exception and the app keeps running; worker
// threads catch at the top and call post_current_exception, which shows the box
// on the UI thread; install() adds SetUnhandledExceptionFilter and
// std::set_terminate for everything else (those end the process after the box).
// MSVC keeps terminate handlers per thread: threads that want terminate()
// reported call install_terminate_handler() first.
//
// The dialog is injectable so tests never show a window.

#include "wizz/core/time.hpp"

#include <QObject>

#include <concepts>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stacktrace>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>

struct _EXCEPTION_POINTERS;

namespace wizz::app {

/// One exception as .NET would print it: ex.Message and ex.ToString().
struct ExceptionReport {
    std::string message;  ///< what() of the outermost exception ("Unknown exception" for non-std types)
    std::string details;  ///< "{type}: {message}", " ---> {inner}" lines, "   at {frame}" lines (CRLF)

    friend bool operator==(const ExceptionReport&, const ExceptionReport&) = default;
};

/// "std::runtime_error" for MSVC's "class std::runtime_error" (class / struct keywords dropped).
[[nodiscard]] std::string exception_type_name(const std::type_info& type);

/// Describes `error` (and the exceptions nested in it) with the frames of `trace`.
/// Frames up to and including the C++ throw machinery (`_CxxThrowException`) are dropped.
[[nodiscard]] ExceptionReport describe_exception(const std::exception_ptr& error, const std::stacktrace& trace);

/// Inside a catch block: the current exception with its throw-site stack trace (recorded
/// on this thread while a reporter is installed), else the stack of this point without
/// this function and `skip_frames` of its callers.
[[nodiscard]] ExceptionReport describe_current_exception(std::size_t skip_frames = 0);

/// An exception caught to be rethrown elsewhere — a coroutine's outcome (task.hpp), a
/// worker job's outcome on the UI thread (job_runner.hpp) — with the stack of its throw site.
class TracedException {
public:
    TracedException() noexcept = default;

    /// Inside a catch block: the current exception and the throw site recorded for it on
    /// this thread (no frames when no reporter is installed).
    [[nodiscard]] static TracedException current() noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(error_); }
    [[nodiscard]] const std::exception_ptr& error() const noexcept { return error_; }
    [[nodiscard]] const std::stacktrace& trace() const noexcept { return trace_; }

    /// Rethrows the exception (which must be set). The carried throw site, not this call,
    /// is recorded as its site: the next catch reports, or carries on, where it was thrown.
    [[noreturn]] void rethrow() const;

private:
    std::exception_ptr error_;
    std::stacktrace trace_;
};

class CrashReporter final : public QObject {
    Q_OBJECT

public:
    /// Shows the crash box: (caption, text), both UTF-8.
    using Dialog = std::function<void(const std::string& caption, const std::string& text)>;

    /// Maximum number of stack frames written to errors.log.
    static constexpr std::size_t kMaxFrames = 64;

    /// The real box: MessageBoxW without owner, MB_OK | MB_ICONERROR, task-modal so the
    /// windows of the calling thread wait for it.
    static void show_message_box(const std::string& caption, const std::string& text);

    /// `clock` (local time of the log record) must outlive the reporter. `data_root`
    /// replaces %LOCALAPPDATA%\WizzairHelper (D-30). An empty `dialog` uses show_message_box.
    /// Construct on the UI thread: posted crashes are shown on the reporter's thread.
    explicit CrashReporter(const Clock& clock, std::optional<std::filesystem::path> data_root = std::nullopt,
                           Dialog dialog = {}, QObject* parent = nullptr);
    /// Uninstalls the process hooks when this reporter installed them.
    ~CrashReporter() override;

    CrashReporter(const CrashReporter&) = delete;
    CrashReporter& operator=(const CrashReporter&) = delete;

    /// Changes the data root after --autotest parsing (thread-safe).
    void set_data_root(std::optional<std::filesystem::path> data_root);
    /// Stamps later records with `clock`, e.g. the --fixed-now clock of D-30 (thread-safe).
    /// Throws std::invalid_argument for a null clock.
    void set_clock(std::shared_ptr<const Clock> clock);
    /// <root>\errors.log. Throws std::system_error when LocalAppData cannot be resolved.
    [[nodiscard]] std::filesystem::path log_path() const;

    /// Appends one record to errors.log. Returns false (and otherwise ignores the
    /// failure) when the directory or file cannot be written. Thread-safe.
    bool append_to_log(std::string_view details) noexcept;

    /// ShowCrash: errors.log record, then the dialog (on the calling thread).
    void show_crash(const ExceptionReport& report) noexcept;

    /// Shows `report` on the reporter's (UI) thread; callable from any thread.
    void post(ExceptionReport report);

    /// Makes this the process-wide reporter: SetUnhandledExceptionFilter, std::set_terminate
    /// (for the calling thread) and the throw-site tracer. Throws std::logic_error when
    /// another reporter is installed; a second call on the same reporter does nothing.
    void install();
    /// Restores the previous filter and terminate handler and removes the tracer (also done
    /// by the destructor).
    void uninstall() noexcept;
    [[nodiscard]] bool is_installed() const noexcept;
    /// The installed reporter, or null.
    [[nodiscard]] static CrashReporter* instance() noexcept;

    /// std::set_terminate(reporting handler) for the calling thread (MSVC keeps one per thread).
    static void install_terminate_handler() noexcept;

    /// Inside a catch block on the UI thread: describes the current exception and shows
    /// it through the installed reporter. False when none is installed.
    static bool report_current_exception() noexcept;
    /// Inside a catch block on any thread: describes the current exception there (with
    /// that thread's stack) and shows it on the UI thread. False when none is installed.
    static bool post_current_exception() noexcept;

    /// Runs `task`; an exception escaping it is reported through the installed reporter
    /// and false is returned (the posted-task executor / event dispatch wrapper). Without
    /// an installed reporter the exception propagates.
    template <std::invocable F>
    static bool run_guarded(F&& task)
    {
        try {
            std::invoke(std::forward<F>(task));
            return true;
        } catch (...) {
            if (!report_current_exception()) {
                throw;
            }
            return false;
        }
    }

    /// The SetUnhandledExceptionFilter callback: a structured exception (access
    /// violation, …) is logged and shown, then EXCEPTION_EXECUTE_HANDLER ends the process.
    /// A C++ exception is passed to the previous (CRT) filter, which calls terminate().
    static long __stdcall unhandled_exception_filter(_EXCEPTION_POINTERS* pointers) noexcept;
    /// ShowCrash input for a structured exception, e.g. "Access violation reading
    /// location 0x0000000000000000.".
    [[nodiscard]] static ExceptionReport describe_structured_exception(const _EXCEPTION_POINTERS& pointers,
                                                                       const std::stacktrace& trace);

private:
    Dialog dialog_;
    mutable std::mutex mutex_;  // guards clock_, data_root_ and the errors.log append
    std::shared_ptr<const Clock> clock_;  // the constructor's clock is not owned
    std::optional<std::filesystem::path> data_root_;
};

} // namespace wizz::app
