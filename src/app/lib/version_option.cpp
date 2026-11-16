#include "version_option.hpp"

#include <algorithm>
#include <cstddef>

#include <windows.h>

namespace wizz::app {

namespace {

/// Writes all of `text` to `handle` (a pipe, file or console). False on any failure.
[[nodiscard]] bool write_all(HANDLE handle, std::string_view text) noexcept
{
    while (!text.empty()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(text.size(), 64 * 1024));
        DWORD written = 0;
        if (!WriteFile(handle, text.data(), chunk, &written, nullptr) || written == 0) {
            return false;
        }
        text.remove_prefix(written);
    }
    return true;
}

/// The standard output handle the creator passed in (STARTF_USESTDHANDLES or inherited),
/// or null when there is none a write could reach.
[[nodiscard]] HANDLE inherited_standard_output() noexcept
{
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    SetLastError(NO_ERROR);
    if (GetFileType(handle) == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR) {
        return nullptr;  // a stale value that names no open handle
    }
    return handle;
}

/// Writes `text` to the console of the parent process (cmd.exe, PowerShell) after
/// attaching to it; the process detaches again afterwards.
[[nodiscard]] bool write_to_parent_console(std::string_view text) noexcept
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return false;  // no parent console, or this process already has one (a test)
    }
    bool written = false;
    const HANDLE console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
    if (console != INVALID_HANDLE_VALUE) {
        written = write_all(console, text);
        CloseHandle(console);
    }
    FreeConsole();
    return written;
}

} // namespace

bool asks_for_version(std::span<const std::string> arguments) noexcept
{
    return std::ranges::find(arguments, std::string_view("--version")) != arguments.end();
}

std::string version_text()
{
    return std::string("WizzairHelper ") + WIZZ_VERSION_STRING + "\n";
}

bool write_standard_output(std::string_view text) noexcept
{
    if (const HANDLE inherited = inherited_standard_output(); inherited != nullptr && write_all(inherited, text)) {
        return true;
    }
    return write_to_parent_console(text);
}

} // namespace wizz::app
