#pragma once
// JobRunner: the threading bridge of 06 §2 and 01 §14.
//
//   * Every job (route map, place resolution, leg search) runs on its own worker
//     thread, even when everything it needs is cached.
//   * The worker talks to the UI only through its JobChannel: `post` queues a
//     closure on the UI thread (Qt::QueuedConnection on the runner, which lives
//     there). All closures of a job and its completion travel through the same
//     queue, so progress, row batches and completion arrive in the order the
//     worker produced them.
//   * Every event carries the job ID. A job is live from its start until its
//     completion has been delivered (or its awaiting flow was destroyed); events of
//     a job that is no longer live are dropped (stale / finished jobs).
//   * JobMailbox coalesces a worker's frequent events (progress, row batches) into
//     one delivery per burst, in order and still ahead of the completion.
//   * Completion resumes the awaiting coroutine on the UI thread: the UI never
//     blocks on a future or joins a running worker. The job's exception is
//     rethrown there with the worker's throw site (TracedException), which a
//     crash record then starts at.
//   * Cancellation: the caller's std::stop_token is linked to the job's own stop
//     source, so one stop request reaches every wait inside the job. Destroying the
//     runner stops and joins all workers (application shutdown).
//
// The runner must live on the UI thread. Its public members are UI-thread only,
// except JobChannel, which belongs to the worker.

#include "crash_reporter.hpp"

#include <QObject>
#include <QPointer>

#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wizz::app {

using JobId = std::uint64_t;

class JobRunner;

/// The worker's side of a job: its ID, its stop token and the ordered channel to the UI.
class JobChannel {
public:
    [[nodiscard]] JobId id() const noexcept { return id_; }
    /// Stops when the caller's token stops or the runner shuts down.
    [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_; }
    /// Queues `on_ui_thread` behind every earlier event of this job. It runs on the UI
    /// thread unless the job is no longer live there. Callable from the worker only.
    void post(std::function<void()> on_ui_thread) const;

private:
    friend class JobRunner;
    JobChannel(JobRunner& runner, JobId id, std::stop_token stop) noexcept : runner_(&runner), id_(id), stop_(std::move(stop)) {}

    JobRunner* runner_;
    JobId id_;
    std::stop_token stop_;
};

/// Coalesces a worker's frequent events into one UI-thread delivery per burst.
///
/// push() appends an item and posts a delivery through the job's channel only when
/// none is pending; the delivery takes every item pushed until it runs, in push order.
/// A worker that produces faster than the UI drains (a fully cached replay posts a
/// progress report and a row batch per request within milliseconds) then costs one
/// queued call per drain instead of one per item, so the event queue empties between
/// drains and the 40 ms progress timer is not starved (05 §5, R-03 / R-04). The
/// delivery travels in the job's FIFO queue like any post(): items pushed before the
/// job returned are delivered before its completion, and nothing is delivered once
/// the job is no longer live. Copies share one mailbox.
template <class T>
class JobMailbox {
public:
    using Deliver = std::function<void(std::vector<T> items)>;

    /// `deliver` runs on the UI thread with the items in push order.
    explicit JobMailbox(Deliver deliver) : state_(std::make_shared<State>()) { state_->deliver = std::move(deliver); }

    /// Worker side: queues `item`; posts a delivery unless one is pending.
    void push(const JobChannel& channel, T item) const
    {
        bool schedule = false;
        {
            const std::scoped_lock lock{state_->mutex};
            state_->items.push_back(std::move(item));
            schedule = !std::exchange(state_->scheduled, true);
        }
        if (schedule) {
            channel.post([state = state_] {
                std::vector<T> items;
                {
                    const std::scoped_lock lock{state->mutex};
                    items.swap(state->items);
                    state->scheduled = false;
                }
                state->deliver(std::move(items));
            });
        }
    }

private:
    struct State {
        std::mutex mutex;
        std::vector<T> items;   ///< guarded by mutex
        bool scheduled = false; ///< guarded by mutex: a delivery is queued
        Deliver deliver;        ///< UI thread
    };
    std::shared_ptr<State> state_;
};

template <class R>
class JobAwaiter;

class JobRunner final : public QObject {
    Q_OBJECT

public:
    explicit JobRunner(QObject* parent = nullptr);
    /// Requests stop on every worker and joins it (shutdown; posted events are dropped).
    ~JobRunner() override;

    JobRunner(const JobRunner&) = delete;
    JobRunner& operator=(const JobRunner&) = delete;

    /// A fresh job ID (increasing, never 0). Reserve it before the job starts when UI
    /// state such as ui::LegStream must know it.
    [[nodiscard]] JobId new_job_id() noexcept { return next_id_++; }

    /// `co_await runner.run(id, stop, work)`: starts `work(channel)` on a new worker
    /// thread when awaited and resumes the awaiting coroutine on the UI thread with its
    /// result (or rethrows its exception). `work` is moved to the worker.
    template <class Work>
        requires std::invocable<Work&, const JobChannel&>
    [[nodiscard]] auto run(JobId id, std::stop_token stop, Work work)
        -> JobAwaiter<std::invoke_result_t<Work&, const JobChannel&>>
    {
        return JobAwaiter<std::invoke_result_t<Work&, const JobChannel&>>{*this, id, std::move(stop), std::move(work)};
    }

    /// Whether events of `id` are still delivered (started and not yet completed).
    [[nodiscard]] bool is_live(JobId id) const { return live_.contains(id); }
    /// Jobs started and not yet completed.
    [[nodiscard]] std::size_t live_count() const noexcept { return live_.size(); }
    /// Worker threads not yet joined (finished ones are joined when the next job starts).
    [[nodiscard]] std::size_t thread_count() const noexcept { return workers_.size(); }

private:
    template <class R>
    friend class JobAwaiter;
    friend class JobChannel;

    struct Worker;

    /// Starts `body` on a worker; `complete` runs on the UI thread after it returned.
    void start(JobId id, std::stop_token stop, std::move_only_function<void(const JobChannel&)> body,
               std::function<void()> complete);
    /// The awaiting flow went away: the job's further events are dropped.
    void abandon(JobId id) noexcept;
    /// Worker side: queue an event of job `id`.
    void post_event(JobId id, std::function<void()> on_ui_thread);
    /// Worker side: queue the completion of job `id` (always delivered, ends liveness).
    void post_completion(JobId id, std::function<void()> complete);
    void reap_finished_workers();

    JobId next_id_ = 1;
    std::unordered_set<JobId> live_;
    std::vector<std::unique_ptr<Worker>> workers_;
};

/// The awaitable of JobRunner::run (lives in the awaiting coroutine's frame).
template <class R>
class [[nodiscard]] JobAwaiter {
public:
    using Work = std::move_only_function<R(const JobChannel&)>;

    JobAwaiter(JobRunner& runner, JobId id, std::stop_token stop, Work work)
        : runner_(&runner), id_(id), stop_(std::move(stop)), work_(std::move(work)), state_(std::make_shared<State>())
    {
    }
    JobAwaiter(const JobAwaiter&) = delete;
    JobAwaiter& operator=(const JobAwaiter&) = delete;
    JobAwaiter(JobAwaiter&&) = default;
    JobAwaiter& operator=(JobAwaiter&&) = delete;
    ~JobAwaiter()
    {
        if (state_ && state_->started && !state_->completed) {
            state_->abandoned = true;  // the frame is being destroyed while the job runs
            if (runner_) {
                runner_->abandon(id_);
            }
        }
    }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> awaiting)
    {
        state_->started = true;
        state_->continuation = awaiting;
        runner_->start(
            id_, stop_,
            [state = state_, work = std::move(work_)](const JobChannel& channel) mutable {
                try {
                    if constexpr (std::is_void_v<R>) {
                        work(channel);
                    } else {
                        state->value.emplace(work(channel));
                    }
                } catch (...) {
                    state->error = TracedException::current();  // with the worker's throw site
                }
            },
            [state = state_] {
                state->completed = true;
                if (!state->abandoned) {
                    state->continuation.resume();
                }
            });
    }

    R await_resume()
    {
        if (state_->error) {
            state_->error.rethrow();
        }
        if constexpr (!std::is_void_v<R>) {
            return std::move(*state_->value);
        }
    }

    [[nodiscard]] JobId id() const noexcept { return id_; }

private:
    struct Empty {};
    struct State {
        // Written by the worker before it posts the completion; read on the UI thread
        // after the completion was delivered (the event queue orders the two).
        std::optional<std::conditional_t<std::is_void_v<R>, Empty, R>> value;
        TracedException error;
        // UI thread only.
        std::coroutine_handle<> continuation;
        bool started = false;
        bool completed = false;
        bool abandoned = false;
    };

    QPointer<JobRunner> runner_;  // null once the runner is gone (shutdown)
    JobId id_;
    std::stop_token stop_;
    Work work_;
    std::shared_ptr<State> state_;
};

} // namespace wizz::app
