#pragma once
// TaskScope: starts the root flows nobody awaits (a Search button, the
// right-click return, the start-up sequence) and owns them until they finish.
//
// An exception that escapes a root flow is a UI-thread exception of 01 §3.2:
// it goes to the installed CrashReporter (errors.log + the crash box) and the
// app keeps running, exactly like an exception escaping a .NET async void
// handler. The flows' own `finally` guards have already ended the busy panel
// with UnexpectedErrorText by then (UI-3).
//
// Destroying the scope destroys the frames of the flows that are still
// suspended (application shutdown while a job runs). UI thread only.

#include "task.hpp"

#include <coroutine>
#include <cstddef>
#include <exception>
#include <functional>
#include <vector>

namespace wizz::app {

class TaskScope {
public:
    /// Called inside the catch block with the escaping exception. Empty = report it
    /// through CrashReporter::report_current_exception (qCritical when none is installed).
    using ErrorHandler = std::function<void(std::exception_ptr error)>;

    TaskScope() = default;
    explicit TaskScope(ErrorHandler on_error) : on_error_(std::move(on_error)) {}
    ~TaskScope();

    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;

    /// Starts `task` now: it runs until its first suspension, and later resumes from
    /// the event loop. Finished roots release themselves.
    void spawn(Task<> task);

    /// Root flows that have not finished yet.
    [[nodiscard]] std::size_t active_count() const noexcept { return roots_.size(); }

    void set_error_handler(ErrorHandler on_error) { on_error_ = std::move(on_error); }

    /// The root coroutine's frame (implementation detail of spawn).
    struct Root;

private:
    friend struct Root;
    /// Awaits `task` and reports what escapes it.
    static Root run_root(TaskScope& scope, Task<> task);
    void add(std::coroutine_handle<> root);
    void remove(std::coroutine_handle<> root) noexcept;
    void handle_error() noexcept;

    ErrorHandler on_error_;
    std::vector<std::coroutine_handle<>> roots_;
    bool destroying_ = false;
};

} // namespace wizz::app
