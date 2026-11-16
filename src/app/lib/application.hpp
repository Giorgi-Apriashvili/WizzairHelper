#pragma once
// The QApplication of WizzairHelper.exe. Qt does not let exceptions travel
// through its event dispatch, so every UI-thread entry point (input, paint,
// timers, queued slot calls and posted tasks) is guarded in notify(): an
// exception that escapes an event handler goes to the installed CrashReporter
// (errors.log + crash box, 01 §3.2) and the application keeps running. Without
// an installed reporter the exception propagates as before.
//
// The constructor makes the application icon (application_icon(), D-51) the icon
// of every window.
//
// notify() also reports each UI-thread dispatch to an optional EventWatchdog
// (the --autotest diagnostics, 05 §7.3 R-03); none is installed normally.

#include <QApplication>

namespace wizz::app {

class EventWatchdog;

class Application final : public QApplication {
    Q_OBJECT

public:
    Application(int& argc, char** argv);

    bool notify(QObject* receiver, QEvent* event) override;

    /// Reports every UI-thread dispatch to `watchdog` from now on (null: stop). The
    /// watchdog must stay alive until it is removed.
    void set_event_watchdog(EventWatchdog* watchdog) noexcept { watchdog_ = watchdog; }
    [[nodiscard]] EventWatchdog* event_watchdog() const noexcept { return watchdog_; }

private:
    EventWatchdog* watchdog_ = nullptr;
};

} // namespace wizz::app
