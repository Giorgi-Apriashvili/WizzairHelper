#include "win32_util.hpp"

#include <climits>
#include <stdexcept>

namespace wizz::win32 {
namespace {

[[nodiscard]] int checked_length(std::size_t size)
{
    if (size > static_cast<std::size_t>(INT_MAX)) {
        throw std::length_error("String too long for a Win32 conversion");
    }
    return static_cast<int>(size);
}

} // namespace

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
