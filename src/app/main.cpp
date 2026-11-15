// WizzairHelper.exe (01 §3): one GUI process per invocation.
//
// Start-up order: crash reporting first (01 §3.2), then the optional --autotest
// command line (01 §17.1, D-30) before the QApplication — a bad --out / --ret /
// --fixed-now value crashes before the window opens — then per-monitor DPI and
// the event loop, the services, settings.json, and the main window.
#include "app_services.hpp"
#include "application.hpp"
#include "crash_reporter.hpp"
#include "main_window.hpp"
#include "ui_style.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/ui/autotest_options.hpp"
#include "wizz/ui/settings.hpp"

#include <QApplication>
#include <QGuiApplication>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <shellapi.h>

namespace {

/// The command-line arguments as UTF-8, without the program path. Read from the
/// UTF-16 command line so non-ASCII places and empty values (`--to ""`) survive.
[[nodiscard]] std::vector<std::string> command_line_arguments()
{
    std::vector<std::string> arguments;
    int count = 0;
    LPWSTR* const wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) {
        return arguments;
    }
    for (int i = 1; i < count; ++i) {
        const int length = static_cast<int>(wcslen(wide[i]));
        std::string utf8;
        if (length > 0) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide[i], length, nullptr, 0, nullptr, nullptr);
            utf8.resize(static_cast<std::size_t>(bytes));
            WideCharToMultiByte(CP_UTF8, 0, wide[i], length, utf8.data(), bytes, nullptr, nullptr);
        }
        arguments.push_back(std::move(utf8));
    }
    LocalFree(wide);
    return arguments;
}

} // namespace

int main(int argc, char* argv[])
{
    using namespace wizz;
    using wizz::app::CrashReporter;

    const SystemClock crash_clock;
    CrashReporter crash_reporter(crash_clock);
    crash_reporter.install();

    std::optional<ui::AutoTestOptions> autotest;
    try {
        autotest = ui::AutoTestOptions::parse(command_line_arguments(), std::filesystem::current_path(),
                                              Locale::user_default()->number_symbols());
    } catch (...) {
        CrashReporter::report_current_exception();  // e.g. a bad --out date: before the window opens
        return 1;
    }
    if (autotest) {
        crash_reporter.set_data_root(autotest->data_root);  // errors.log of the isolated profile (D-30)
    }

    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    app::Application application(argc, argv);
    app::style::apply_application_look();
    try {
        const std::shared_ptr<const app::AppServices> services =
            app::AppServices::create(app::ServiceConfig::for_run(autotest));
        ui::AppSettings settings = ui::AppSettings::load(services->settings_path());  // before the window (01 §3)
        app::MainWindow window(services, std::move(settings), autotest);
        window.show_main_window();
        return QApplication::exec();
    } catch (...) {
        CrashReporter::report_current_exception();  // start-up failures outside the event loop
        return 1;
    }
}
