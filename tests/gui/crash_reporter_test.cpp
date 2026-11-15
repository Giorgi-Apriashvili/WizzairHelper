// CrashReporter (01 §3.2, §18.1): errors.log records in a temporary data root, the
// exact crash box text through an injected dialog, exception descriptions, the
// UI-thread guard, worker posting, the process hooks and the terminate path.

#include "application.hpp"
#include "crash_reporter.hpp"
#include "diagnostics_test_support.hpp"

#include "wizz/core/clocks.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <windows.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace wizz_crash_test {

struct ParseFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

__declspec(noinline) void throw_from_named_function()
{
    throw ParseFailure("Wizz Air sent a timetable that could not be read.");
}

/// An event handler that fails, like a slot that throws on the UI thread.
class FailingReceiver final : public QObject {
public:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::User) {
            throw std::runtime_error("boom");
        }
        return QObject::event(event);
    }
};

} // namespace wizz_crash_test

namespace {

namespace fs = std::filesystem;
using wizz::FixedClock;
using wizz::app::CrashReporter;
using wizz::app::ExceptionReport;

constexpr std::string_view kCrashText =
    "Something went wrong:\n\nboom\n\nDetails were written to %LOCALAPPDATA%\\WizzairHelper\\errors.log.";

[[nodiscard]] std::string read_file(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// Records dialog calls from any thread.
struct DialogRecorder {
    struct Shown {
        std::string caption;
        std::string text;
        QThread* thread = nullptr;
    };

    [[nodiscard]] CrashReporter::Dialog dialog()
    {
        return [this](const std::string& caption, const std::string& text) {
            const std::scoped_lock lock(mutex);
            shown.push_back({caption, text, QThread::currentThread()});
        };
    }
    [[nodiscard]] std::vector<Shown> snapshot()
    {
        const std::scoped_lock lock(mutex);
        return shown;
    }

    std::mutex mutex;
    std::vector<Shown> shown;
};

class TempRoot {
public:
    TempRoot()
    {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("wizz_crash_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++counter));
        fs::remove_all(path_);
    }
    ~TempRoot()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;

    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

class CrashReporterTest : public ::testing::Test {
protected:
    // 2026-11-02T10:00:00Z, local zone UTC+1: the record carries local 11:00:00.
    FixedClock clock_{FixedClock::kDefaultUtcNow, std::chrono::hours{1}};
    TempRoot root_;
    DialogRecorder recorder_;
    CrashReporter reporter_{clock_, root_.path(), recorder_.dialog()};
};

// ---------------------------------------------------------------------------
// errors.log and the crash box (01 §3.2)
// ---------------------------------------------------------------------------

TEST_F(CrashReporterTest, Log_path_is_errors_log_in_the_data_root)
{
    EXPECT_EQ(reporter_.log_path(), root_.path() / "errors.log");
}

TEST_F(CrashReporterTest, Show_crash_appends_the_record_and_shows_the_exact_box)
{
    reporter_.show_crash({.message = "boom", .details = "std::runtime_error: boom\r\n   at f()"});

    EXPECT_EQ(read_file(root_.path() / "errors.log"), "[2026-11-02T11:00:00] std::runtime_error: boom\r\n   at f()\r\n\r\n");
    const auto shown = recorder_.snapshot();
    ASSERT_EQ(shown.size(), 1U);
    EXPECT_EQ(shown[0].caption, "WizzairHelper");
    EXPECT_EQ(shown[0].text, kCrashText);
}

TEST_F(CrashReporterTest, Records_are_appended)
{
    reporter_.show_crash({.message = "one", .details = "A: one"});
    clock_.advance(std::chrono::seconds{3});
    reporter_.show_crash({.message = "two", .details = "B: two"});

    EXPECT_EQ(read_file(root_.path() / "errors.log"),
              "[2026-11-02T11:00:00] A: one\r\n\r\n[2026-11-02T11:00:03] B: two\r\n\r\n");
    EXPECT_EQ(recorder_.snapshot().size(), 2U);
}

TEST(CrashReporterFiles, Missing_directories_are_created)
{
    const TempRoot root;
    const FixedClock clock;
    DialogRecorder recorder;
    CrashReporter reporter(clock, root.path() / "nested" / "profile", recorder.dialog());
    EXPECT_TRUE(reporter.append_to_log("X: y"));
    EXPECT_EQ(read_file(root.path() / "nested" / "profile" / "errors.log"), "[2026-11-02T10:00:00] X: y\r\n\r\n");
}

TEST(CrashReporterFiles, Write_failures_are_ignored_and_the_box_still_shows)
{
    const TempRoot root;
    fs::create_directories(root.path());
    const fs::path blocker = root.path() / "not-a-directory";
    std::ofstream(blocker) << "file";
    const FixedClock clock;
    DialogRecorder recorder;
    CrashReporter reporter(clock, blocker, recorder.dialog());  // <file>\errors.log cannot exist

    EXPECT_FALSE(reporter.append_to_log("X: y"));
    reporter.show_crash({.message = "boom", .details = "X: boom"});
    const auto shown = recorder.snapshot();
    ASSERT_EQ(shown.size(), 1U);
    EXPECT_EQ(shown[0].text, kCrashText);
}

TEST_F(CrashReporterTest, The_data_root_can_change_after_start_up)
{
    const TempRoot other;
    reporter_.set_data_root(other.path());
    EXPECT_EQ(reporter_.log_path(), other.path() / "errors.log");
    reporter_.set_data_root(std::nullopt);
    EXPECT_EQ(reporter_.log_path().filename(), "errors.log");
    EXPECT_EQ(reporter_.log_path().parent_path().filename(), "WizzairHelper");  // %LOCALAPPDATA%\WizzairHelper
}

// ---------------------------------------------------------------------------
// Exception descriptions (ex.Message / ex.ToString())
// ---------------------------------------------------------------------------

TEST(ExceptionDescription, Type_and_message)
{
    const ExceptionReport report =
        wizz::app::describe_exception(std::make_exception_ptr(std::runtime_error("boom")), std::stacktrace{});
    EXPECT_EQ(report.message, "boom");
    EXPECT_EQ(report.details, "std::runtime_error: boom");

    const ExceptionReport own = wizz::app::describe_exception(
        std::make_exception_ptr(wizz_crash_test::ParseFailure("bad")), std::stacktrace{});
    EXPECT_EQ(own.details, "wizz_crash_test::ParseFailure: bad");
}

TEST(ExceptionDescription, Type_names_drop_msvc_keywords)
{
    EXPECT_EQ(wizz::app::exception_type_name(typeid(std::runtime_error)), "std::runtime_error");
    EXPECT_EQ(wizz::app::exception_type_name(typeid(wizz_crash_test::ParseFailure)), "wizz_crash_test::ParseFailure");
    EXPECT_EQ(wizz::app::exception_type_name(typeid(std::vector<std::runtime_error>)),
              "std::vector<std::runtime_error,std::allocator<std::runtime_error> >");
}

TEST(ExceptionDescription, Nested_exceptions_are_inner_exceptions)
{
    std::exception_ptr error;
    try {
        try {
            throw std::invalid_argument("inner");
        } catch (...) {
            std::throw_with_nested(std::runtime_error("outer"));
        }
    } catch (...) {
        error = std::current_exception();
    }
    const ExceptionReport report = wizz::app::describe_exception(error, std::stacktrace{});
    EXPECT_EQ(report.message, "outer");
    EXPECT_EQ(report.details, "std::runtime_error: outer\r\n"
                              " ---> std::invalid_argument: inner\r\n"
                              "   --- End of inner exception stack trace ---");
}

TEST(ExceptionDescription, Non_standard_exceptions_are_unknown)
{
    const ExceptionReport report = wizz::app::describe_exception(std::make_exception_ptr(42), std::stacktrace{});
    EXPECT_EQ(report.message, "Unknown exception");
    EXPECT_EQ(report.details, "Unknown exception");
}

TEST_F(CrashReporterTest, The_stack_trace_starts_at_the_throw_site)
{
#ifdef NDEBUG
    GTEST_SKIP() << "Release builds carry no PDB, so frames have no function names or lines.";
#endif
    reporter_.install();  // the throw-site tracer
    ExceptionReport report;
    try {
        wizz_crash_test::throw_from_named_function();
    } catch (...) {
        report = wizz::app::describe_current_exception();
    }
    reporter_.uninstall();
    EXPECT_EQ(report.message, "Wizz Air sent a timetable that could not be read.");
    const std::string head = "wizz_crash_test::ParseFailure: Wizz Air sent a timetable that could not be read.\r\n   at ";
    ASSERT_TRUE(report.details.starts_with(head)) << report.details;
    const std::string first_frame = report.details.substr(head.size(), report.details.find("\r\n", head.size()) - head.size());
    EXPECT_NE(first_frame.find("throw_from_named_function"), std::string::npos) << report.details;
    EXPECT_NE(first_frame.find("crash_reporter_test.cpp:line "), std::string::npos) << report.details;
    EXPECT_EQ(report.details.find("CxxThrowException"), std::string::npos) << report.details;
}

// ---------------------------------------------------------------------------
// Hooks: UI-thread guard, worker posting, process-wide handlers
// ---------------------------------------------------------------------------

TEST_F(CrashReporterTest, Install_sets_and_uninstall_restores_the_process_hooks)
{
    const std::terminate_handler terminate_before = std::get_terminate();
    reporter_.install();
    EXPECT_TRUE(reporter_.is_installed());
    EXPECT_EQ(CrashReporter::instance(), &reporter_);
    EXPECT_NE(std::get_terminate(), terminate_before);
    const LPTOP_LEVEL_EXCEPTION_FILTER filter = SetUnhandledExceptionFilter(nullptr);
    EXPECT_EQ(filter, &CrashReporter::unhandled_exception_filter);
    SetUnhandledExceptionFilter(filter);

    reporter_.install();  // idempotent
    const FixedClock clock;
    CrashReporter second(clock);
    EXPECT_THROW(second.install(), std::logic_error);

    reporter_.uninstall();
    EXPECT_FALSE(reporter_.is_installed());
    EXPECT_EQ(CrashReporter::instance(), nullptr);
    EXPECT_EQ(std::get_terminate(), terminate_before);
}

TEST_F(CrashReporterTest, Run_guarded_reports_and_keeps_running)
{
    reporter_.install();
    bool ran_after = false;
    const bool completed = CrashReporter::run_guarded([] { throw std::runtime_error("boom"); });
    ran_after = CrashReporter::run_guarded([] {});
    reporter_.uninstall();

    EXPECT_FALSE(completed);
    EXPECT_TRUE(ran_after);
    const auto shown = recorder_.snapshot();
    ASSERT_EQ(shown.size(), 1U);
    EXPECT_EQ(shown[0].text, kCrashText);
    EXPECT_TRUE(read_file(root_.path() / "errors.log").starts_with("[2026-11-02T11:00:00] std::runtime_error: boom\r\n   at "));
}

TEST_F(CrashReporterTest, Application_reports_exceptions_escaping_event_handlers_and_keeps_running)
{
    ASSERT_NE(dynamic_cast<wizz::app::Application*>(QCoreApplication::instance()), nullptr);
    reporter_.install();
    wizz_crash_test::FailingReceiver receiver;
    bool delivered_after = false;
    QCoreApplication::postEvent(&receiver, new QEvent(QEvent::User));  // Qt takes ownership
    QMetaObject::invokeMethod(&receiver, [&delivered_after] { delivered_after = true; }, Qt::QueuedConnection);
    QCoreApplication::sendPostedEvents(&receiver);
    reporter_.uninstall();

    EXPECT_TRUE(delivered_after);  // the event loop went on
    const auto shown = recorder_.snapshot();
    ASSERT_EQ(shown.size(), 1U);
    EXPECT_EQ(shown[0].text, kCrashText);
    const std::string log = read_file(root_.path() / "errors.log");
    EXPECT_TRUE(log.starts_with("[2026-11-02T11:00:00] std::runtime_error: boom\r\n   at ")) << log;
#ifndef NDEBUG
    EXPECT_NE(log.find("FailingReceiver::event"), std::string::npos) << log;  // the throw site
#endif
}

TEST(CrashReporterHooks, Without_a_reporter_run_guarded_rethrows)
{
    ASSERT_EQ(CrashReporter::instance(), nullptr);
    EXPECT_THROW(CrashReporter::run_guarded([] { throw std::logic_error("x"); }), std::logic_error);
    EXPECT_FALSE(CrashReporter::post_current_exception());
}

TEST_F(CrashReporterTest, A_worker_exception_is_shown_on_the_ui_thread)
{
    reporter_.install();
    bool posted = false;
    std::thread worker([&posted] {
        try {
            throw std::runtime_error("boom");
        } catch (...) {
            posted = CrashReporter::post_current_exception();
        }
    });
    worker.join();
    EXPECT_TRUE(posted);
    EXPECT_TRUE(recorder_.snapshot().empty());  // nothing shown on the worker

    QCoreApplication::sendPostedEvents(&reporter_);
    reporter_.uninstall();

    const auto shown = recorder_.snapshot();
    ASSERT_EQ(shown.size(), 1U);
    EXPECT_EQ(shown[0].text, kCrashText);
    EXPECT_EQ(shown[0].thread, QCoreApplication::instance()->thread());
    EXPECT_TRUE(read_file(root_.path() / "errors.log").starts_with("[2026-11-02T11:00:00] std::runtime_error: boom"));
}

TEST_F(CrashReporterTest, A_posted_crash_is_dropped_with_its_reporter)
{
    {
        const FixedClock clock;
        DialogRecorder recorder;
        auto reporter = std::make_unique<CrashReporter>(clock, root_.path(), recorder.dialog());
        reporter->post({.message = "late", .details = "X: late"});
        reporter.reset();
        QCoreApplication::sendPostedEvents();
        EXPECT_TRUE(recorder.snapshot().empty());
    }
}

TEST_F(CrashReporterTest, Structured_exceptions_are_logged_and_shown_by_the_filter)
{
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = reinterpret_cast<PVOID>(0x1234);
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 0;  // read
    record.ExceptionInformation[1] = 0;
    CONTEXT context{};
    EXCEPTION_POINTERS pointers{&record, &context};

    reporter_.install();
    const long verdict = CrashReporter::unhandled_exception_filter(&pointers);
    reporter_.uninstall();

    EXPECT_EQ(verdict, EXCEPTION_EXECUTE_HANDLER);
    const auto shown = recorder_.snapshot();
    ASSERT_EQ(shown.size(), 1U);
    EXPECT_EQ(shown[0].text, "Something went wrong:\n\nAccess violation reading location 0x0000000000000000.\n\n"
                             "Details were written to %LOCALAPPDATA%\\WizzairHelper\\errors.log.");
    EXPECT_TRUE(read_file(root_.path() / "errors.log")
                    .starts_with("[2026-11-02T11:00:00] Win32 exception 0xC0000005 at 0x0000000000001234: "
                                 "Access violation reading location 0x0000000000000000."));
}

TEST(StructuredExceptionDescription, Messages_name_the_fault)
{
    EXCEPTION_RECORD record{};
    CONTEXT context{};
    EXCEPTION_POINTERS pointers{&record, &context};
    const auto message = [&] { return CrashReporter::describe_structured_exception(pointers, std::stacktrace{}).message; };

    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 1;
    record.ExceptionInformation[1] = 0xDEADBEEF;
    EXPECT_EQ(message(), "Access violation writing location 0x00000000DEADBEEF.");
    record.ExceptionInformation[0] = 8;
    EXPECT_EQ(message(), "Access violation executing location 0x00000000DEADBEEF.");
    record.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
    EXPECT_EQ(message(), "Integer division by zero.");
    record.ExceptionCode = EXCEPTION_STACK_OVERFLOW;
    EXPECT_EQ(message(), "Stack overflow.");
    record.ExceptionCode = 0xC0000409;
    EXPECT_EQ(message(), "Unhandled exception 0xC0000409.");
}

// The terminate handler logs, shows the box and ends the process (exit code 3, as abort()).
TEST(CrashReporterDeathTest, Terminate_is_reported_before_the_process_ends)
{
    // gtest runs this body again in a child process, which must use the parent's
    // directory: it arrives through the inherited environment.
    std::optional<TempRoot> root;
    if (!qEnvironmentVariableIsSet("WIZZ_CRASH_TEST_ROOT")) {
        root.emplace();
        qputenv("WIZZ_CRASH_TEST_ROOT", QString::fromStdWString(root->path().wstring()).toUtf8());
    }
    EXPECT_EXIT(
        {
            const FixedClock clock;
            CrashReporter reporter(clock, fs::path(qEnvironmentVariable("WIZZ_CRASH_TEST_ROOT").toStdWString()),
                                   [](const std::string&, const std::string&) {});
            reporter.install();
            try {
                throw std::runtime_error("fatal");
            } catch (...) {
                std::terminate();
            }
        },
        ::testing::ExitedWithCode(3), "");
    qunsetenv("WIZZ_CRASH_TEST_ROOT");
    ASSERT_TRUE(root.has_value());
    const std::string log = read_file(root->path() / "errors.log");
    EXPECT_TRUE(log.starts_with("[2026-11-02T10:00:00] std::runtime_error: fatal\r\n   at ")) << log;
#ifndef NDEBUG
    // The throw site, recorded by the tracer although the catch had unwound it.
    EXPECT_NE(log.find("Terminate_is_reported_before_the_process_ends"), std::string::npos) << log;
#endif
}

} // namespace
