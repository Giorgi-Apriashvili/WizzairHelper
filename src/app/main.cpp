// Placeholder entry point; the desktop experience (01) is wired in the GUI milestone.
// Crash reporting (01 §3.2) is installed before anything else can fail.
#include "app_info.hpp"
#include "application.hpp"
#include "crash_reporter.hpp"

#include "wizz/core/clocks.hpp"

#include <QMainWindow>

int main(int argc, char* argv[])
{
    wizz::app::Application app(argc, argv);
    const wizz::SystemClock clock;
    wizz::app::CrashReporter crash_reporter(clock);
    crash_reporter.install();
    try {
        QMainWindow window;
        window.setWindowTitle(wizz::app::main_window_title());
        window.show();
        return QApplication::exec();
    } catch (...) {
        // Start-up failures outside the event loop (e.g. a bad --autotest date).
        wizz::app::CrashReporter::report_current_exception();
        return 1;
    }
}
