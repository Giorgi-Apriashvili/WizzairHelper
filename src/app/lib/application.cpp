#include "application.hpp"

#include "crash_reporter.hpp"

namespace wizz::app {

Application::Application(int& argc, char** argv) : QApplication(argc, argv) {}

bool Application::notify(QObject* receiver, QEvent* event)
{
    bool handled = false;
    CrashReporter::run_guarded([&] { handled = QApplication::notify(receiver, event); });
    return handled;
}

} // namespace wizz::app
