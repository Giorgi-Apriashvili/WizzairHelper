#pragma once
// Small helpers for the progress / diagnostics widget tests.

#include "cpu_meter.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <chrono>
#include <cstdint>
#include <functional>

namespace wizz::test {

/// Runs the event loop for `duration` (timers fire, posted events are delivered).
inline void pump_for(std::chrono::milliseconds duration)
{
    QEventLoop loop;
    QTimer::singleShot(duration, &loop, &QEventLoop::quit);
    loop.exec();
}

/// Runs the event loop until `done` or `timeout`; returns `done()`.
inline bool pump_until(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds{5})
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return done();
}

/// Keeps the calling thread busy until it has used `ms` of its own CPU time: UI work of a
/// known size, however long a busy machine takes to give it that much CPU.
inline void spin_cpu(double ms)
{
    using ::wizz::app::CpuMeter;
    const std::uint64_t start = CpuMeter::thread_cycles();
    const double cycles = ms * CpuMeter::cycles_per_ms();
    while (static_cast<double>(CpuMeter::thread_cycles() - start) < cycles) {
    }
}

/// Milliseconds between two readings of the steady clock.
[[nodiscard]] inline double ms_between(std::chrono::steady_clock::time_point from,
                                       std::chrono::steady_clock::time_point to)
{
    return std::chrono::duration<double, std::milli>(to - from).count();
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
