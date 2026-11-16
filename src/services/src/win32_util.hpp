#pragma once
// Small Win32 helpers shared by the services: UTF-8 <-> UTF-16 conversion at the
// API boundary (ARCHITECTURE "Strings") and RAII owners for Win32 handles.

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace wizz::win32 {

/// UTF-8 → UTF-16 for Win32 calls; ill-formed input becomes U+FFFD.
[[nodiscard]] std::wstring to_wide(std::string_view utf8);

/// UTF-16 → UTF-8; unpaired surrogates become U+FFFD.
[[nodiscard]] std::string to_utf8(std::wstring_view utf16);

/// Creates or truncates `path` opened exclusively (a concurrent writer of the same file
/// fails instead of interleaving, 06 §3), writes `content` and flushes it to disk, so a
/// rename can publish it. Returns the Win32 error code, ERROR_SUCCESS on success.
[[nodiscard]] DWORD write_new_file(const std::filesystem::path& path, std::string_view content);

/// The system's text for a Win32 error code (UTF-8, trailing period and white space
/// removed), e.g. "Access is denied"; "Windows error <n>" when there is none.
[[nodiscard]] std::string error_text(DWORD code);

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
