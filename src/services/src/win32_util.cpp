#include "win32_util.hpp"

#include <algorithm>
#include <climits>
#include <format>
#include <stdexcept>

namespace wizz::win32 {
namespace {

constexpr DWORD kIoChunk = 1u << 20;

[[nodiscard]] int checked_length(std::size_t size)
{
    if (size > static_cast<std::size_t>(INT_MAX)) {
        throw std::length_error("String too long for a Win32 conversion");
    }
    return static_cast<int>(size);
}

struct LocalFreer {
    void operator()(wchar_t* memory) const noexcept { ::LocalFree(memory); }
};

} // namespace

DWORD write_new_file(const std::filesystem::path& path, std::string_view content)
{
    const HANDLE raw =
        ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return ::GetLastError();
    }
    const UniqueHandle file{raw};
    while (!content.empty()) {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(content.size(), kIoChunk));
        DWORD written = 0;
        if (::WriteFile(raw, content.data(), chunk, &written, nullptr) == FALSE) {
            return ::GetLastError();
        }
        if (written == 0) {
            return ERROR_WRITE_FAULT;
        }
        content.remove_prefix(written);
    }
    // Make the data durable before a rename publishes it.
    return ::FlushFileBuffers(raw) != FALSE ? ERROR_SUCCESS : ::GetLastError();
}

std::string error_text(DWORD code)
{
    wchar_t* raw = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<wchar_t*>(&raw), 0, nullptr); // ALLOCATE_BUFFER: the argument receives the buffer
    const std::unique_ptr<wchar_t, LocalFreer> owner{raw};
    std::wstring_view text{raw, raw == nullptr ? 0 : length};
    while (!text.empty() && (text.back() == L'.' || text.back() == L' ' || text.back() == L'\r' || text.back() == L'\n')) {
        text.remove_suffix(1);
    }
    return text.empty() ? std::format("Windows error {}", code) : to_utf8(text);
}

std::wstring to_wide(std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int length = checked_length(utf8.size());
    const int required = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), length, wide.data(), required);
    return wide;
}

std::string to_utf8(std::wstring_view utf16)
{
    if (utf16.empty()) {
        return {};
    }
    const int length = checked_length(utf16.size());
    const int required = ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), length, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(required), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, utf16.data(), length, utf8.data(), required, nullptr, nullptr);
    return utf8;
}

} // namespace wizz::win32
