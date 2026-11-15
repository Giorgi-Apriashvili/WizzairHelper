#pragma once
// Task<T>: the UI thread's `async` (01 §11 "await resumes on the UI thread").
//
// The flows of 01 §11 are written as C++ coroutines that mirror the .NET
// async methods line by line: `co_await` a child flow or a worker job
// (JobRunner), `co_return` a value, try/catch around the awaits, RAII guards
// for the `finally` blocks. A Task is lazy: its body starts when it is awaited
// (symmetric transfer, no extra event-loop round trip), and when it finishes
// it transfers straight back to the awaiting coroutine. Exceptions travel
// through `co_await` like through `await`.
//
// Everything runs on the thread that resumes the coroutines, which is the UI
// thread: worker jobs post their completion back to it (job_runner.hpp). A
// root flow that nobody awaits (a button click) is started by
// TaskScope::spawn (task_scope.hpp), which also reports escaping exceptions.
//
// A Task owns its coroutine frame; destroying an unfinished Task destroys the
// frame (and, through its locals, the frames of the tasks it awaits).

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace wizz::app {

template <class T = void>
class Task;

namespace detail {

struct TaskPromiseBase {
    /// The coroutine that awaits this task; resumed when the body finishes.
    std::coroutine_handle<> continuation = std::noop_coroutine();
    std::exception_ptr error;

    struct FinalAwaiter {
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        template <class Promise>
        [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> finished) const noexcept
        {
            return finished.promise().continuation;
        }
        void await_resume() const noexcept {}
    };

    [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
    [[nodiscard]] FinalAwaiter final_suspend() const noexcept { return {}; }
    void unhandled_exception() noexcept { error = std::current_exception(); }
};

template <class T>
struct TaskPromise final : TaskPromiseBase {
    std::optional<T> value;

    [[nodiscard]] Task<T> get_return_object() noexcept;

    template <class U>
        requires std::is_constructible_v<T, U&&>
    void return_value(U&& result)
    {
        value.emplace(std::forward<U>(result));
    }

    [[nodiscard]] T take()
    {
        if (error) {
            std::rethrow_exception(error);
        }
        return std::move(*value);
    }
};

template <>
struct TaskPromise<void> final : TaskPromiseBase {
    [[nodiscard]] Task<void> get_return_object() noexcept;
    void return_void() const noexcept {}

    void take() const
    {
        if (error) {
            std::rethrow_exception(error);
        }
    }
};

} // namespace detail

template <class T>
class [[nodiscard]] Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(Handle handle) noexcept : handle_(handle) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { reset(); }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(handle_); }
    /// The body ran to its end (value or exception).
    [[nodiscard]] bool done() const noexcept { return handle_ && handle_.done(); }

    /// Starts the body without an awaiting coroutine (tests, TaskScope); it runs up to
    /// its first suspension. Resuming a started or finished task is a logic error.
    void start() { handle_.resume(); }

    /// The result of a finished task; rethrows its exception.
    decltype(auto) result()
    {
        return handle_.promise().take();
    }

    /// `co_await task`: runs the body (symmetric transfer) and resumes the awaiting
    /// coroutine when it finishes; yields the value or rethrows the exception.
    auto operator co_await() && noexcept
    {
        struct Awaiter {
            Handle handle;
            [[nodiscard]] bool await_ready() const noexcept { return !handle || handle.done(); }
            [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) const noexcept
            {
                handle.promise().continuation = awaiting;
                return handle;
            }
            decltype(auto) await_resume() const { return handle.promise().take(); }
        };
        return Awaiter{handle_};
    }

private:
    void reset() noexcept
    {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    Handle handle_;
};

namespace detail {

template <class T>
Task<T> TaskPromise<T>::get_return_object() noexcept
{
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept
{
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

} // namespace detail

} // namespace wizz::app
