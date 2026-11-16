#include "installer_launcher.hpp"

#include "grid_actions.hpp"

#include <QString>

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <thread>

namespace wizz::app {

namespace {

/// Process and thread handles of a started process, closed at once: the app never waits.
struct StartedProcess {
    PROCESS_INFORMATION info{};
    StartedProcess() = default;
    StartedProcess(const StartedProcess&) = delete;
    StartedProcess& operator=(const StartedProcess&) = delete;
    ~StartedProcess()
    {
        if (info.hThread != nullptr) {
            ::CloseHandle(info.hThread);
        }
        if (info.hProcess != nullptr) {
            ::CloseHandle(info.hProcess);
        }
    }
};

/// ShellExecuteExW for a program that needs elevation (UAC). Like the grids' browser
/// call it runs on its own STA thread, so the shell never spins the UI thread's loop.
[[nodiscard]] DWORD shell_execute(const std::wstring& program, const std::wstring& parameters,
                                  const std::wstring& directory)
{
    DWORD error = ERROR_SUCCESS;
    std::jthread worker{[&] {
        const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW info{};
        info.cbSize = static_cast<DWORD>(sizeof(info));
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"open";
        info.lpFile = program.c_str();
        info.lpParameters = parameters.c_str();
        info.lpDirectory = directory.empty() ? nullptr : directory.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (::ShellExecuteExW(&info) == FALSE) {
            error = ::GetLastError();
            if (error == ERROR_SUCCESS) {
                error = ERROR_GEN_FAILURE;
            }
        }
        if (SUCCEEDED(com)) {
            ::CoUninitialize();
        }
    }};
    worker.join();
    return error;
}

} // namespace

std::expected<void, std::string> launch_detached(const update::InstallerCommand& command)
{
    const std::wstring program = command.program.wstring();
    const std::wstring directory = command.program.parent_path().wstring();
    // Not inherited handles, a process group of its own, no error boxes inherited from
    // the app, and outside the app's job object, so closing the app (or a launcher that
    // kills its job) never takes the installer down with it.
    constexpr DWORD kFlags = CREATE_NEW_PROCESS_GROUP | CREATE_DEFAULT_ERROR_MODE;
    const auto create = [&](DWORD flags, StartedProcess& process) {
        std::wstring line = command.command_line();  // CreateProcessW may write into it
        STARTUPINFOW startup{};
        startup.cb = static_cast<DWORD>(sizeof(startup));
        return ::CreateProcessW(program.c_str(), line.data(), nullptr, nullptr, FALSE, flags, nullptr,
                                directory.empty() ? nullptr : directory.c_str(), &startup, &process.info) != FALSE;
    };

    StartedProcess process;
    if (create(kFlags | CREATE_BREAKAWAY_FROM_JOB, process)) {
        return {};
    }
    DWORD error = ::GetLastError();
    if (error == ERROR_ACCESS_DENIED) {
        // A job that does not allow breaking away: start the installer inside it.
        if (create(kFlags, process)) {
            return {};
        }
        error = ::GetLastError();
    }
    if (error == ERROR_ELEVATION_REQUIRED) {
        error = shell_execute(program, QString::fromStdString(command.parameters()).toStdWString(), directory);
        if (error == ERROR_SUCCESS) {
            return {};
        }
    }
    return std::unexpected(system_message(error).toStdString());
}

} // namespace wizz::app
