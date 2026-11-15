#include "task_scope.hpp"

#include "crash_reporter.hpp"

#include <QtGlobal>

#include <algorithm>
#include <utility>

namespace wizz::app {

/// The detached coroutine that awaits one root task: it starts suspended so the
/// scope can register it before it runs, and frees its frame when it ends.
struct TaskScope::Root {
    struct promise_type {
        TaskScope* scope = nullptr;

        [[nodiscard]] Root get_return_object() noexcept
        {
            return Root{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
        [[nodiscard]] std::suspend_never final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        // The body catches everything; an exception here would be a bug in handle_error.
        void unhandled_exception() const noexcept { std::terminate(); }

        promise_type() = default;
        promise_type(const promise_type&) = delete;
        promise_type& operator=(const promise_type&) = delete;
        ~promise_type()
        {
            if (scope != nullptr) {
                scope->remove(std::coroutine_handle<promise_type>::from_promise(*this));
            }
        }
    };

    std::coroutine_handle<promise_type> handle;
};

TaskScope::Root TaskScope::run_root(TaskScope& scope, Task<> task)
{
    try {
        co_await std::move(task);
    } catch (...) {
        scope.handle_error();
    }
}

TaskScope::~TaskScope()
{
    destroying_ = true;
    // Destroying a suspended root destroys its task and, through the task's frame,
    // every flow it awaits; their guards see the shutdown and touch nothing.
    std::vector<std::coroutine_handle<>> roots = std::exchange(roots_, {});
    for (std::coroutine_handle<> root : roots) {
        root.destroy();
    }
}

void TaskScope::spawn(Task<> task)
{
    Root root = run_root(*this, std::move(task));
    root.handle.promise().scope = this;
    add(root.handle);
    root.handle.resume();  // may finish at once; the frame then removes itself
}

void TaskScope::add(std::coroutine_handle<> root)
{
    roots_.push_back(root);
}

void TaskScope::remove(std::coroutine_handle<> root) noexcept
{
    if (destroying_) {
        return;
    }
    std::erase(roots_, root);
}

void TaskScope::handle_error() noexcept
{
    try {
        if (on_error_) {
            on_error_(std::current_exception());
            return;
        }
        if (!CrashReporter::report_current_exception()) {
            qCritical("An exception escaped a UI flow and no crash reporter is installed.");
        }
    } catch (...) {
        qCritical("The UI flow error handler failed.");
    }
}

} // namespace wizz::app
