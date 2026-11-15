#include "crash_reporter.hpp"

#include "wizz/services/app_paths.hpp"
#include "wizz/ui/strings.hpp"

#include <QMetaObject>
#include <QString>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace wizz::app {

namespace {

/// The SEH code of every MSVC C++ throw ('msc').
constexpr DWORD kMsvcCppException = 0xE06D7363;
/// STATUS_HEAP_CORRUPTION (not in <windows.h>).
constexpr DWORD kHeapCorruption = 0xC0000374;

// Process-wide hook state, written by install / uninstall, read by the hooks on any thread.
std::atomic<CrashReporter*> g_installed{nullptr};
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_previous_filter{nullptr};
std::terminate_handler g_previous_terminate = nullptr;  // only install / uninstall use it
std::atomic<PVOID> g_throw_tracer{nullptr};

/// Room for the exception machinery frames that frame_lines drops.
constexpr std::size_t kCapturedFrames = CrashReporter::kMaxFrames + 48;

/// The stack of the latest C++ throw on this thread. MSVC x64 runs a catch block only
/// after the throwing frames have been unwound, so the throw site must be recorded while
/// the exception is raised: the vectored handler sees every throw before any catch.
thread_local std::stacktrace t_last_throw;

/// The original throw site of the exception that TracedException::rethrow raises next on
/// this thread: the tracer records it instead of the rethrow's own stack.
thread_local std::optional<std::stacktrace> t_carried_throw;

LONG CALLBACK record_throw_site(EXCEPTION_POINTERS* pointers) noexcept
{
    const EXCEPTION_RECORD* record = pointers != nullptr ? pointers->ExceptionRecord : nullptr;
    if (record == nullptr || record->ExceptionCode != kMsvcCppException || record->NumberParameters < 3) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (record->ExceptionInformation[1] == 0) {
        return EXCEPTION_CONTINUE_SEARCH;  // `throw;` re-raises the current exception: keep its site
    }
    if (t_carried_throw) {
        t_last_throw = std::move(*t_carried_throw);
        t_carried_throw.reset();
        return EXCEPTION_CONTINUE_SEARCH;
    }
    try {
        t_last_throw = std::stacktrace::current(0, kCapturedFrames);
    } catch (...) {
        t_last_throw = std::stacktrace{};
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/// The trace for the exception being handled: the latest throw site recorded on this
/// thread while the tracer is installed, else the current stack.
[[nodiscard]] std::stacktrace handled_exception_trace(std::size_t skip_frames)
{
    if (g_throw_tracer.load() != nullptr && !t_last_throw.empty()) {
        return t_last_throw;
    }
    return std::stacktrace::current(skip_frames + 1, kCapturedFrames);
}

[[nodiscard]] bool contains(std::string_view text, std::string_view part) noexcept
{
    return text.find(part) != std::string_view::npos;
}

/// "   at {module!function+0x..}[ in {file}:line {n}]" (.NET StackTrace line shape).
[[nodiscard]] std::string frame_line(const std::stacktrace_entry& entry, const std::string& description)
{
    std::string line = "   at ";
    if (description.empty()) {
        line += std::format("0x{:016X}", reinterpret_cast<std::uintptr_t>(entry.native_handle()));
    } else {
        line += description;
    }
    if (std::string file = entry.source_file(); !file.empty()) {
        line += std::format(" in {}:line {}", file, entry.source_line());
    }
    return line;
}

/// The "   at …" lines of `trace`, starting at the throw site: frames up to and including
/// `_CxxThrowException` (C++ throw) or, failing that, `KiUserExceptionDispatcher` (SEH)
/// belong to the exception machinery and the reporter.
[[nodiscard]] std::vector<std::string> frame_lines(const std::stacktrace& trace)
{
    std::vector<std::string> descriptions;
    descriptions.reserve(trace.size());
    std::optional<std::size_t> cpp_throw;
    std::optional<std::size_t> dispatcher;
    for (std::size_t i = 0; i < trace.size(); ++i) {
        descriptions.push_back(trace[i].description());
        if (!cpp_throw && contains(descriptions.back(), "CxxThrowException")) {
            cpp_throw = i;
        }
        if (!dispatcher && contains(descriptions.back(), "KiUserExceptionDispatcher")) {
            dispatcher = i;
        }
    }
    std::size_t first = 0;
    if (cpp_throw) {
        first = *cpp_throw + 1;
    } else if (dispatcher) {
        first = *dispatcher + 1;
    }
    std::vector<std::string> lines;
    for (std::size_t i = first; i < trace.size() && lines.size() < CrashReporter::kMaxFrames; ++i) {
        lines.push_back(frame_line(trace[i], descriptions[i]));
    }
    return lines;
}

/// The type shown for `e`: std::throw_with_nested(T) throws an unspecified class derived
/// from T and std::nested_exception (MSVC: std::_With_nested_v2<T>), which is named after T.
[[nodiscard]] std::string shown_type_name(const std::exception& e)
{
    std::string name = exception_type_name(typeid(e));
    if (name.starts_with("std::_With_nested")) {
        const std::size_t open = name.find('<');
        const std::size_t close = name.rfind('>');
        if (open != std::string::npos && close != std::string::npos && close > open + 1) {
            name = name.substr(open + 1, close - open - 1);
            while (!name.empty() && name.back() == ' ') {
                name.pop_back();
            }
        }
    }
    return name;
}

void append_frames(std::string& details, const std::stacktrace& trace)
{
    for (const std::string& line : frame_lines(trace)) {
        details += "\r\n";
        details += line;
    }
}

[[nodiscard]] std::wstring to_wide(const std::string& utf8)
{
    return QString::fromStdString(utf8).toStdWString();
}

[[noreturn]] void reporting_terminate() noexcept
{
    static std::atomic_flag entered;
    if (!entered.test_and_set()) {
        if (CrashReporter* reporter = CrashReporter::instance()) {
            ExceptionReport report;
            try {
                if (const std::exception_ptr error = std::current_exception()) {
                    report = describe_exception(error, handled_exception_trace(0));
                } else {
                    report.message = "std::terminate was called.";
                    report.details = "std::terminate: " + report.message;
                    append_frames(report.details, std::stacktrace::current(0, kCapturedFrames));
                }
            } catch (...) {
                report.message = "std::terminate was called.";
                report.details = report.message;
            }
            reporter->show_crash(report);
        }
    }
    // The box was the report: no "abort() has been called" box and no WER dialog on top.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::abort();
}

} // namespace

// ---------------------------------------------------------------------------
// Describing exceptions
// ---------------------------------------------------------------------------

std::string exception_type_name(const std::type_info& type)
{
    std::string name = type.name();
    // MSVC prints "class std::runtime_error", "struct Foo", also inside template arguments.
    for (const std::string_view keyword : {std::string_view{"class "}, std::string_view{"struct "},
                                           std::string_view{"union "}, std::string_view{"enum "}}) {
        std::size_t position = 0;
        while ((position = name.find(keyword, position)) != std::string::npos) {
            const bool starts_token = position == 0 || name[position - 1] == '<' || name[position - 1] == ',' ||
                                      name[position - 1] == ' ' || name[position - 1] == '(';
            if (starts_token) {
                name.erase(position, keyword.size());
            } else {
                position += keyword.size();
            }
        }
    }
    return name;
}

ExceptionReport describe_exception(const std::exception_ptr& error, const std::stacktrace& trace)
{
    ExceptionReport report;
    std::size_t levels = 0;
    std::exception_ptr current = error;
    while (current) {
        std::exception_ptr inner;
        std::string type;
        std::string message;
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            type = shown_type_name(e);
            message = e.what();
            if (const auto* nested = dynamic_cast<const std::nested_exception*>(&e)) {
                inner = nested->nested_ptr();
            }
        } catch (const std::nested_exception& nested) {
            type = exception_type_name(typeid(nested));
            inner = nested.nested_ptr();
        } catch (...) {
            type = "Unknown exception";
        }

        std::string line = message.empty() ? type : type + ": " + message;
        if (levels == 0) {
            report.message = message.empty() && type == "Unknown exception" ? type : message;
            report.details = std::move(line);
        } else {
            report.details += "\r\n ---> ";
            report.details += line;
        }
        ++levels;
        current = std::move(inner);
    }
    if (levels == 0) {
        report.message = "Unknown exception";
        report.details = report.message;
    }
    for (std::size_t i = 1; i < levels; ++i) {
        report.details += "\r\n   --- End of inner exception stack trace ---";
    }
    append_frames(report.details, trace);
    return report;
}

ExceptionReport describe_current_exception(std::size_t skip_frames)
{
    // Read the recorded site first: describe_exception rethrows, which records again.
    const std::stacktrace trace = handled_exception_trace(skip_frames + 1);
    return describe_exception(std::current_exception(), trace);
}

// ---------------------------------------------------------------------------
// TracedException
// ---------------------------------------------------------------------------

TracedException TracedException::current() noexcept
{
    TracedException caught;
    caught.error_ = std::current_exception();
    if (caught.error_ && g_throw_tracer.load() != nullptr) {
        try {
            caught.trace_ = t_last_throw;
        } catch (...) {
            caught.trace_ = std::stacktrace{};  // no memory for the frames: the exception still travels
        }
    }
    return caught;
}

void TracedException::rethrow() const
{
    if (error_ && !trace_.empty() && g_throw_tracer.load() != nullptr) {
        try {
            t_carried_throw = trace_;  // taken by record_throw_site for the raise below
        } catch (...) {
            t_carried_throw.reset();
        }
    }
    std::rethrow_exception(error_);
}

// ---------------------------------------------------------------------------
// CrashReporter
// ---------------------------------------------------------------------------

void CrashReporter::show_message_box(const std::string& caption, const std::string& text)
{
    const std::wstring wide_caption = to_wide(caption);
    const std::wstring wide_text = to_wide(text);
    // No owner (01 §18.1); MB_TASKMODAL disables this thread's windows while it is open.
    static_cast<void>(MessageBoxW(nullptr, wide_text.c_str(), wide_caption.c_str(),
                                  MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND));
}

CrashReporter::CrashReporter(const Clock& clock, std::optional<std::filesystem::path> data_root, Dialog dialog,
                             QObject* parent)
    : QObject(parent),
      dialog_(std::move(dialog)),
      clock_(std::shared_ptr<const Clock>{}, &clock),  // aliasing, owns nothing
      data_root_(std::move(data_root))
{
}

CrashReporter::~CrashReporter()
{
    uninstall();
}

void CrashReporter::set_data_root(std::optional<std::filesystem::path> data_root)
{
    const std::scoped_lock lock(mutex_);
    data_root_ = std::move(data_root);
}

void CrashReporter::set_clock(std::shared_ptr<const Clock> clock)
{
    if (!clock) {
        throw std::invalid_argument("CrashReporter::set_clock needs a clock.");
    }
    const std::scoped_lock lock(mutex_);
    clock_ = std::move(clock);
}

std::filesystem::path CrashReporter::log_path() const
{
    std::optional<std::filesystem::path> root;
    {
        const std::scoped_lock lock(mutex_);
        root = data_root_;
    }
    return app_paths::errors_log(root);
}

bool CrashReporter::append_to_log(std::string_view details) noexcept
{
    try {
        const std::scoped_lock lock(mutex_);
        const std::filesystem::path path = app_paths::errors_log(data_root_);
        const std::string entry = ui::strings::errors_log_entry(clock_->local_now(), details);
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
        std::ofstream file(path, std::ios::binary | std::ios::app);
        if (!file) {
            return false;
        }
        file.write(entry.data(), static_cast<std::streamsize>(entry.size()));
        file.close();
        return !file.fail();
    } catch (...) {
        return false;  // IOException / UnauthorizedAccessException are ignored (01 §3.2)
    }
}

void CrashReporter::show_crash(const ExceptionReport& report) noexcept
{
    static_cast<void>(append_to_log(report.details));
    try {
        const std::string caption{ui::strings::kAppCaption};
        const std::string text = ui::strings::crash_message(report.message);
        if (dialog_) {
            dialog_(caption, text);
        } else {
            show_message_box(caption, text);
        }
    } catch (...) {
        // Nothing sensible is left to do when the crash box itself fails.
    }
}

void CrashReporter::post(ExceptionReport report)
{
    QMetaObject::invokeMethod(
        this, [this, report = std::move(report)] { show_crash(report); }, Qt::QueuedConnection);
}

void CrashReporter::install()
{
    CrashReporter* expected = nullptr;
    if (!g_installed.compare_exchange_strong(expected, this)) {
        if (expected == this) {
            return;
        }
        throw std::logic_error("Another CrashReporter is already installed.");
    }
    g_previous_filter = SetUnhandledExceptionFilter(&CrashReporter::unhandled_exception_filter);
    g_previous_terminate = std::set_terminate(&reporting_terminate);
    g_throw_tracer = AddVectoredExceptionHandler(0, &record_throw_site);
}

void CrashReporter::uninstall() noexcept
{
    CrashReporter* expected = this;
    if (!g_installed.compare_exchange_strong(expected, nullptr)) {
        return;
    }
    if (const PVOID tracer = g_throw_tracer.exchange(nullptr); tracer != nullptr) {
        RemoveVectoredExceptionHandler(tracer);
    }
    SetUnhandledExceptionFilter(g_previous_filter.exchange(nullptr));
    std::set_terminate(std::exchange(g_previous_terminate, nullptr));
}

bool CrashReporter::is_installed() const noexcept
{
    return g_installed.load() == this;
}

CrashReporter* CrashReporter::instance() noexcept
{
    return g_installed.load();
}

void CrashReporter::install_terminate_handler() noexcept
{
    std::set_terminate(&reporting_terminate);
}

bool CrashReporter::report_current_exception() noexcept
{
    CrashReporter* reporter = instance();
    if (reporter == nullptr) {
        return false;
    }
    ExceptionReport report;
    try {
        report = describe_current_exception(1);
    } catch (...) {
        report = {.message = "Unknown exception", .details = "Unknown exception"};
    }
    reporter->show_crash(report);
    return true;
}

bool CrashReporter::post_current_exception() noexcept
{
    CrashReporter* reporter = instance();
    if (reporter == nullptr) {
        return false;
    }
    try {
        reporter->post(describe_current_exception(1));
    } catch (...) {
        return false;
    }
    return true;
}

ExceptionReport CrashReporter::describe_structured_exception(const _EXCEPTION_POINTERS& pointers,
                                                             const std::stacktrace& trace)
{
    const EXCEPTION_RECORD& record = *pointers.ExceptionRecord;
    const DWORD code = record.ExceptionCode;
    std::string message;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        if (record.NumberParameters >= 2) {
            const ULONG_PTR operation = record.ExceptionInformation[0];  // 0 read, 1 write, 8 DEP
            const char* verb = operation == 1 ? "writing" : operation == 8 ? "executing" : "reading";
            message = std::format("Access violation {} location 0x{:016X}.", verb, record.ExceptionInformation[1]);
        } else {
            message = "Access violation.";
        }
        break;
    case EXCEPTION_STACK_OVERFLOW:
        message = "Stack overflow.";
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        message = "Integer division by zero.";
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        message = "Illegal instruction.";
        break;
    case EXCEPTION_PRIV_INSTRUCTION:
        message = "Privileged instruction.";
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        message = "In-page I/O error.";
        break;
    case kHeapCorruption:
        message = "Heap corruption.";
        break;
    case kMsvcCppException:
        message = "Unhandled C++ exception.";
        break;
    default:
        message = std::format("Unhandled exception 0x{:08X}.", code);
        break;
    }
    ExceptionReport report;
    report.details = std::format("Win32 exception 0x{:08X} at 0x{:016X}: {}", code,
                                 reinterpret_cast<std::uintptr_t>(record.ExceptionAddress), message);
    report.message = std::move(message);
    append_frames(report.details, trace);
    return report;
}

long __stdcall CrashReporter::unhandled_exception_filter(_EXCEPTION_POINTERS* pointers) noexcept
{
    CrashReporter* reporter = instance();
    const LPTOP_LEVEL_EXCEPTION_FILTER previous = g_previous_filter.load();
    const bool usable = reporter != nullptr && pointers != nullptr && pointers->ExceptionRecord != nullptr;
    if (!usable || (pointers->ExceptionRecord->ExceptionCode == kMsvcCppException && previous != nullptr)) {
        // A C++ exception nobody caught: the CRT filter calls terminate(), which reports it
        // with its type and message.
        return previous != nullptr ? previous(pointers) : EXCEPTION_CONTINUE_SEARCH;
    }
    const bool stack_overflow = pointers->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW;
    try {
        // The faulting thread's frames; skipped after a stack overflow, where this stack is spent.
        const std::stacktrace trace = stack_overflow ? std::stacktrace{} : std::stacktrace::current(0, kCapturedFrames);
        // Report from a fresh thread: it has a stack of its own.
        std::jthread reporting{[reporter, pointers, &trace] {
            ExceptionReport report;
            try {
                report = describe_structured_exception(*pointers, trace);
            } catch (...) {
                report = {.message = "Unhandled exception.", .details = "Unhandled exception."};
            }
            reporter->show_crash(report);
        }};
        reporting.join();
    } catch (...) {
        // Could not report (no memory, no thread): fall through to ending the process.
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace wizz::app
