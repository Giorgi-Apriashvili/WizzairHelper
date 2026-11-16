#include "application.hpp"

#include "app_info.hpp"
#include "crash_reporter.hpp"
#include "event_watchdog.hpp"

#include <QScopeGuard>
#include <QThread>

namespace wizz::app {

Application::Application(int& argc, char** argv) : QApplication(argc, argv)
{
    // Every window and dialog shows the application icon (D-51) unless it sets its own.
    setWindowIcon(application_icon());
}

bool Application::notify(QObject* receiver, QEvent* event)
{
    EventWatchdog* const watchdog = watchdog_;
    if (watchdog == nullptr || QThread::currentThread() != thread()) {
        bool handled = false;
        CrashReporter::run_guarded([&] { handled = QApplication::notify(receiver, event); });
        return handled;
    }
    watchdog->enter(receiver, event);  // before dispatch: the receiver may be deleted by it
    const auto leave = qScopeGuard([watchdog] { watchdog->leave(); });
    bool handled = false;
    CrashReporter::run_guarded([&] { handled = QApplication::notify(receiver, event); });
    return handled;
}

} // namespace wizz::app
