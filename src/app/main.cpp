// WizzairHelper.exe (01 §3): one GUI process per invocation.
//
// Start-up order: per-monitor-v2 DPI awareness comes with the process (application
// manifest, resources/WizzairHelper.manifest). `--version` (D-35) prints the version
// and ends the process before anything else. Then crash reporting (01 §3.2), the
// installer's AppMutex (D-36, D-47), the optional --autotest command line (01 §17.1,
// D-30) before the QApplication — a bad --out / --ret / --fixed-now value crashes
// before the window opens, logged in the --data-root profile — then the event loop,
// the font warm-up (font_warmup.hpp), the services, settings.json, the main window
// and, in a normal run, the self-updater (D-45).
#include "app_mutex.hpp"
#include "app_services.hpp"
#include "application.hpp"
#include "controller.hpp"
#include "crash_reporter.hpp"
#include "font_warmup.hpp"
#include "main_window.hpp"
#include "ui_style.hpp"
#include "update_controller.hpp"
#include "version_option.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/services/updater.hpp"
#include "wizz/ui/autotest_options.hpp"
#include "wizz/ui/settings.hpp"
#include "wizz/ui/update_model.hpp"

#include <QApplication>
#include <QGuiApplication>

#include <exception>
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

    const std::vector<std::string> arguments = command_line_arguments();
    if (app::asks_for_version(arguments)) {
        // No window, settings, network or AppMutex: an updater may ask a new copy for its
        // version while the installed one is still running (D-35).
        app::write_standard_output(app::version_text());
        return 0;
    }
    const SystemClock crash_clock;
    CrashReporter crash_reporter(crash_clock);
    crash_reporter.install();
    // Held until the process exits, or until the update installer is about to start:
    // the installer (AppMutex=WizzairHelper.AppMutex) sees that the app runs (D-47).
    app::AppMutex app_mutex{update::kAppMutexName};

    std::optional<ui::AutoTestOptions> autotest;
    try {
        const std::filesystem::path current_dir = std::filesystem::current_path();
        // errors.log of the isolated profile (D-30), read first so that a bad value
        // below is logged there too.
        crash_reporter.set_data_root(ui::AutoTestOptions::parse_data_root(arguments, current_dir));
        autotest = ui::AutoTestOptions::parse(arguments, current_dir, Locale::user_default()->number_symbols());
    } catch (...) {
        CrashReporter::report_current_exception();  // e.g. a bad --out date: before the window opens
        return 1;
    }
    if (autotest && autotest->fixed_now) {
        // Records carry the run's fixed instant, local zone UTC, like the rest of the run (D-30).
        crash_reporter.set_clock(std::make_shared<const FixedClock>(*autotest->fixed_now));
    }

    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    app::Application application(argc, argv);
    app::style::apply_application_look();
    // The wait note's "⏳" needs Windows' font fallback (0.2-0.4 s the first time): resolve
    // it on a worker while the services and the window are built, and before any busy
    // period can show it (R-03, font_warmup.hpp).
    app::FontWarmup font_warmup;
    try {
        const std::shared_ptr<const app::AppServices> services =
            app::AppServices::create(app::ServiceConfig::for_run(autotest));
        ui::AppSettings settings = ui::AppSettings::load(services->settings_path());  // before the window (01 §3)
        app::MainWindow window(services, std::move(settings), autotest);
        if (ui::updates_allowed(arguments)) {  // never with --autotest or --replay (D-45)
            try {
                window.enable_updates(app::UpdateHooks::system(*services, [&app_mutex] { app_mutex.release(); }));
            } catch (const std::exception& error) {
                // The app works without its updater; say why in the Log only.
                window.controller()->log(ui::update_strings::check_failed_log(error.what()));
            }
        }
        font_warmup.wait();
        window.show_main_window();
        return QApplication::exec();
    } catch (...) {
        CrashReporter::report_current_exception();  // start-up failures outside the event loop
        return 1;
    }
}
