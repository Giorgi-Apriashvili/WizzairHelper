// Task<T> / TaskScope (task.hpp, task_scope.hpp): the coroutine form of the
// 01 §11 async flows — lazy start, values and exceptions through co_await,
// root flows that report escaping exceptions, and frames destroyed on shutdown.

#include "task.hpp"
#include "task_scope.hpp"

#include <gtest/gtest.h>

#include <coroutine>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wizz::app::Task;
using wizz::app::TaskScope;

/// An awaitable that parks the coroutine until the test resumes it.
struct Gate {
    std::coroutine_handle<> waiting;

    struct Awaiter {
        Gate& gate;
        [[nodiscard]] bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) noexcept { gate.waiting = handle; }
        void await_resume() const noexcept {}
    };
    [[nodiscard]] Awaiter wait() noexcept { return Awaiter{*this}; }
    void open()
    {
        std::exchange(waiting, {}).resume();
    }
};

/// Counts its destructions (frame locals of destroyed coroutines).
struct Tracker {
    int* destroyed;
    explicit Tracker(int* counter) : destroyed(counter) {}
    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;
    ~Tracker() { ++*destroyed; }
};

Task<int> answer(std::vector<std::string>& trace)
{
    trace.push_back("answer");
    co_return 42;
}

Task<int> twice(std::vector<std::string>& trace)
{
    trace.push_back("twice: before");
    const int value = co_await answer(trace);
    trace.push_back("twice: after");
    co_return value * 2;
}

Task<int> throws_logic_error()
{
    throw std::logic_error("boom");
    co_return 0;  // unreachable, makes this a coroutine
}

Task<std::string> rethrown()
{
    try {
        co_await throws_logic_error();
    } catch (const std::logic_error& error) {
        co_return std::string{"caught "} + error.what();
    }
    co_return "not reached";
}

Task<> parked(Gate& gate, int* destroyed, std::vector<std::string>& trace)
{
    const Tracker tracker{destroyed};
    trace.push_back("parked: waiting");
    co_await gate.wait();
    trace.push_back("parked: resumed");
}

Task<> nested_parked(Gate& gate, int* destroyed, std::vector<std::string>& trace)
{
    const Tracker tracker{destroyed};
    co_await parked(gate, destroyed, trace);
    trace.push_back("nested: done");
}

TEST(Task, A_task_is_lazy_and_returns_its_value_through_co_await)
{
    std::vector<std::string> trace;
    Task<int> task = twice(trace);
    EXPECT_TRUE(trace.empty()) << "nothing runs before the task is started or awaited";
    EXPECT_FALSE(task.done());
    task.start();
    ASSERT_TRUE(task.done());
    EXPECT_EQ(task.result(), 84);
    EXPECT_EQ(trace, (std::vector<std::string>{"twice: before", "answer", "twice: after"}));
}

TEST(Task, An_exception_travels_through_co_await)
{
    Task<std::string> task = rethrown();
    task.start();
    ASSERT_TRUE(task.done());
    EXPECT_EQ(task.result(), "caught boom");

    Task<int> failing = throws_logic_error();
    failing.start();
    ASSERT_TRUE(failing.done());
    EXPECT_THROW((void)failing.result(), std::logic_error);
}

TEST(Task, A_suspended_task_resumes_its_awaiting_coroutine_when_it_finishes)
{
    Gate gate;
    int destroyed = 0;
    std::vector<std::string> trace;
    Task<> task = nested_parked(gate, &destroyed, trace);
    task.start();
    EXPECT_FALSE(task.done());
    EXPECT_EQ(trace, std::vector<std::string>{"parked: waiting"});
    gate.open();
    EXPECT_TRUE(task.done());
    EXPECT_EQ(trace, (std::vector<std::string>{"parked: waiting", "parked: resumed", "nested: done"}));
    EXPECT_EQ(destroyed, 2);
}

TEST(Task, Destroying_a_suspended_task_destroys_the_frames_it_awaits)
{
    Gate gate;
    int destroyed = 0;
    std::vector<std::string> trace;
    {
        Task<> task = nested_parked(gate, &destroyed, trace);
        task.start();
        EXPECT_EQ(destroyed, 0);
    }
    EXPECT_EQ(destroyed, 2) << "both frames' locals were destroyed";
    EXPECT_EQ(trace, std::vector<std::string>{"parked: waiting"});
}

TEST(TaskScope, A_root_that_finishes_at_once_releases_itself)
{
    TaskScope scope;
    std::vector<std::string> trace;
    scope.spawn([](std::vector<std::string>& out) -> Task<> {
        out.push_back(std::to_string(co_await answer(out)));
    }(trace));
    EXPECT_EQ(scope.active_count(), 0u);
    EXPECT_EQ(trace, (std::vector<std::string>{"answer", "42"}));
}

TEST(TaskScope, A_suspended_root_stays_until_it_finishes)
{
    TaskScope scope;
    Gate gate;
    int destroyed = 0;
    std::vector<std::string> trace;
    scope.spawn(parked(gate, &destroyed, trace));
    EXPECT_EQ(scope.active_count(), 1u);
    gate.open();
    EXPECT_EQ(scope.active_count(), 0u);
    EXPECT_EQ(destroyed, 1);
}

TEST(TaskScope, An_escaping_exception_goes_to_the_error_handler_and_the_scope_goes_on)
{
    std::vector<std::string> errors;
    TaskScope scope{[&errors](std::exception_ptr error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            errors.emplace_back(e.what());
        }
    }};
    scope.spawn([]() -> Task<> {
        co_await throws_logic_error();
    }());
    EXPECT_EQ(errors, std::vector<std::string>{"boom"});
    EXPECT_EQ(scope.active_count(), 0u);

    std::vector<std::string> trace;
    scope.spawn([](std::vector<std::string>& out) -> Task<> {
        out.push_back(std::to_string(co_await answer(out)));
    }(trace));
    EXPECT_EQ(trace.back(), "42");
}

TEST(TaskScope, Destroying_the_scope_destroys_suspended_roots)
{
    Gate gate;
    int destroyed = 0;
    std::vector<std::string> trace;
    {
        TaskScope scope;
        scope.spawn(nested_parked(gate, &destroyed, trace));
        scope.spawn(parked(gate, &destroyed, trace));  // the gate now holds the second one
        EXPECT_EQ(scope.active_count(), 2u);
    }
    EXPECT_EQ(destroyed, 3) << "every frame of both roots";
}

} // namespace
