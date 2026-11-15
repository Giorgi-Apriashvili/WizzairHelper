#pragma once
// Small Win32 helpers shared by the services: UTF-8 <-> UTF-16 conversion at the
// API boundary (ARCHITECTURE "Strings") and RAII owners for Win32 handles.

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>

namespace wizz::win32 {

/// UTF-8 → UTF-16 for Win32 calls; ill-formed input becomes U+FFFD.
[[nodiscard]] std::wstring to_wide(std::string_view utf8);

/// UTF-16 → UTF-8; unpaired surrogates become U+FFFD.
[[nodiscard]] std::string to_utf8(std::wstring_view utf16);

struct HandleCloser {
    void operator()(HANDLE handle) const noexcept { ::CloseHandle(handle); }
};
/// Owns a kernel object handle. Construct it only from a valid handle, never from
/// INVALID_HANDLE_VALUE.
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct GlobalFreer {
    void operator()(wchar_t* memory) const noexcept { ::GlobalFree(memory); }
};
/// Owns a string the WinHTTP proxy APIs allocated with GlobalAlloc.
using GlobalString = std::unique_ptr<wchar_t, GlobalFreer>;

} // namespace wizz::win32
