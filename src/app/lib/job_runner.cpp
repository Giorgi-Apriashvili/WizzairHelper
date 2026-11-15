#include "job_runner.hpp"

#include "crash_reporter.hpp"

#include <QMetaObject>

#include <algorithm>

namespace wizz::app {

/// One worker thread. Heap-allocated so the thread can flag its own end.
struct JobRunner::Worker {
    /// Forwards the caller's stop request to the job's own stop source.
    struct StopForwarder {
        mutable std::stop_source target;  // request_stop is not const
        void operator()() const noexcept { target.request_stop(); }
    };

    JobId id = 0;
    std::atomic<bool> finished{false};
    std::jthread thread;
    std::optional<std::stop_callback<StopForwarder>> link;
};

void JobChannel::post(std::function<void()> on_ui_thread) const
{
    runner_->post_event(id_, std::move(on_ui_thread));
}

JobRunner::JobRunner(QObject* parent) : QObject(parent) {}

JobRunner::~JobRunner()
{
    for (const std::unique_ptr<Worker>& worker : workers_) {
        worker->link.reset();
        worker->thread.request_stop();
    }
    workers_.clear();  // std::jthread joins; their late posts die with this QObject
}

void JobRunner::start(JobId id, std::stop_token stop, std::move_only_function<void(const JobChannel&)> body,
                      std::function<void()> complete)
{
    reap_finished_workers();
    live_.insert(id);

    auto worker = std::make_unique<Worker>();
    worker->id = id;
    Worker* const self = worker.get();
    worker->thread = std::jthread{
        [this, self, id, body = std::move(body), complete = std::move(complete)](std::stop_token own_stop) mutable {
            CrashReporter::install_terminate_handler();  // per thread in the MSVC runtime
            try {
                const JobChannel channel{*this, id, own_stop};
                body(channel);  // the job's own exceptions are captured for the awaiting flow
            } catch (...) {
                CrashReporter::post_current_exception();
            }
            post_completion(id, std::move(complete));
            self->finished.store(true, std::memory_order_release);
        }};
    // Linked after the start: a stop requested before this line is forwarded at once.
    worker->link.emplace(std::move(stop), Worker::StopForwarder{worker->thread.get_stop_source()});
    workers_.push_back(std::move(worker));
}

void JobRunner::abandon(JobId id) noexcept
{
    live_.erase(id);
}

void JobRunner::post_event(JobId id, std::function<void()> on_ui_thread)
{
    QMetaObject::invokeMethod(
        this,
        [this, id, on_ui_thread = std::move(on_ui_thread)] {
            if (live_.contains(id)) {
                on_ui_thread();
            }
        },
        Qt::QueuedConnection);
}

void JobRunner::post_completion(JobId id, std::function<void()> complete)
{
    QMetaObject::invokeMethod(
        this,
        [this, id, complete = std::move(complete)] {
            live_.erase(id);  // later events of this job are stale
            complete();
        },
        Qt::QueuedConnection);
}

void JobRunner::reap_finished_workers()
{
    std::erase_if(workers_, [](const std::unique_ptr<Worker>& worker) {
        return worker->finished.load(std::memory_order_acquire);
    });
}

} // namespace wizz::app
