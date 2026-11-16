#pragma once
// Small helpers for the progress / diagnostics widget tests, and the event-loop waits
// every GUI test uses (gui::wait_until is pump_until).

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QImage>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <chrono>
#include <functional>

namespace wizz::test {

/// The cap of a wait for a condition. A wait returns as soon as its condition holds, so a
/// generous cap costs nothing in a passing run; it only bounds how long a broken test
/// hangs. A loaded CI runner (4 shared vCPUs, 8 test processes, Debug builds) can be many
/// times slower than a desktop, so no wait for a condition uses a short one.
inline constexpr std::chrono::milliseconds kWaitCap{60'000};

/// Runs the event loop until `done()` or `timeout`; returns whether `done()` held.
///
/// The loop blocks between events (no busy loop taking the CPU from the threads a test
/// waits for) and checks `done()` every 5 ms. A true `done()` is latched: the result is
/// what the check saw. Re-reading `done()` after the loop has quit is not the same: the
/// dispatcher may deliver more pending events before exec() returns (measured under load:
/// a 20 ms timer's next tick), so a state that holds only between two events would read
/// false and a passing wait would report a failure long before its cap. The deadline is
/// measured by the poll itself, and a loop quit that did not come from here is re-entered.
inline bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = kWaitCap)
{
    if (done()) {
        return true;
    }
    const QDeadlineTimer deadline{timeout, Qt::PreciseTimer};
    bool held = false;
    QEventLoop loop;
    QTimer poll;
    poll.setTimerType(Qt::PreciseTimer);
    poll.setInterval(std::chrono::milliseconds{5});
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done()) {
            held = true;
            loop.quit();
        } else if (deadline.hasExpired()) {
            loop.quit();
        }
    });
    poll.start();
    while (!held && !deadline.hasExpired()) {
        loop.exec();
    }
    return held || done();
}

/// Runs the event loop for `duration` (timers fire, posted events are delivered). Use it
/// only where the elapsed time itself is the point (a window in which nothing may happen),
/// never as a precondition: wait for the condition with pump_until instead.
inline void pump_for(std::chrono::milliseconds duration)
{
    static_cast<void>(pump_until([] { return false; }, duration));
}

/// Delivers every event already posted to this thread: posts a marker behind them and runs
/// the event loop until it arrives (a thread's posted events are delivered in order). After
/// it, nothing posted before the call can still arrive — the deterministic replacement for
/// "pump a little and hope the worker's post got through".
inline bool drain_posted_events()
{
    bool arrived = false;
    QObject marker;
    static_cast<void>(QMetaObject::invokeMethod(&marker, [&arrived] { arrived = true; }, Qt::QueuedConnection));
    return pump_until([&arrived] { return arrived; });
}

/// Saves `image` as <WIZZ_GUI_SNAPSHOT_DIR>/<name>.png when that variable is set, so the
/// rendering can be inspected by eye (run the tests with the "windows" platform for real fonts).
inline void save_snapshot(const QImage& image, const QString& name)
{
    const QString directory = qEnvironmentVariable("WIZZ_GUI_SNAPSHOT_DIR");
    if (directory.isEmpty()) {
        return;
    }
    QDir().mkpath(directory);
    image.save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

inline void save_snapshot(QWidget& widget, const QString& name)
{
    save_snapshot(widget.grab().toImage(), name);
}

} // namespace wizz::test
