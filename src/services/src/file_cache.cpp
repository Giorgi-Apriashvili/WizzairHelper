#include "wizz/services/file_cache.hpp"

#include "win32_util.hpp"

#include "wizz/core/text_decoding.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <system_error>

namespace wizz {
namespace {

/// FILETIME counts 100 ns ticks (one wizz::Tick) since 1601-01-01 UTC, without leap
/// seconds — the same scale as UtcInstant, so the conversion is exact.
constexpr std::chrono::sys_days kFileTimeEpoch{std::chrono::year{1601} / std::chrono::January / 1};

constexpr DWORD kIoChunk = 1u << 20;

[[nodiscard]] UtcInstant to_utc_instant(const FILETIME& time) noexcept
{
    const std::uint64_t ticks = (std::uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    return UtcInstant{kFileTimeEpoch} + Ticks{static_cast<std::int64_t>(ticks)};
}

/// The whole file, opened like File.ReadAllText (shared for reading only); nullopt on
/// any I/O or access error.
[[nodiscard]] std::optional<std::string> read_all_bytes(const std::filesystem::path& path)
{
    const HANDLE raw = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    const win32::UniqueHandle file{raw};
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(raw, &size) == FALSE) {
        return std::nullopt;
    }
    // One spare byte lets the loop notice a file that grew since GetFileSizeEx.
    std::string bytes(static_cast<std::size_t>(size.QuadPart) + 1, '\0');
    std::size_t filled = 0;
    for (;;) {
        if (filled == bytes.size()) {
            bytes.resize(bytes.size() * 2);
        }
        const auto wanted = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - filled, kIoChunk));
        DWORD read = 0;
        if (::ReadFile(raw, bytes.data() + filled, wanted, &read, nullptr) == FALSE) {
            return std::nullopt;
        }
        if (read == 0) {
            bytes.resize(filled);
            return bytes;
        }
        filled += read;
    }
}

} // namespace

std::optional<std::filesystem::path> FileCache::path_of(std::string_view name) const
{
    if (!directory_) {
        return std::nullopt;
    }
    return *directory_ / win32::to_wide(name);
}

std::optional<std::string> FileCache::try_read(std::string_view name, std::optional<Duration> max_age,
                                               const Clock& clock) const
{
    const auto path = path_of(name);
    if (!path) {
        return std::nullopt;
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (::GetFileAttributesExW(path->c_str(), GetFileExInfoStandard, &attributes) == FALSE ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return std::nullopt; // missing (File.Exists is false for directories)
    }
    if (max_age && clock.utc_now() - to_utc_instant(attributes.ftLastWriteTime) > *max_age) {
        return std::nullopt;
    }
    const auto bytes = read_all_bytes(*path);
    if (!bytes) {
        return std::nullopt;
    }
    return decode_text(*bytes);
}

void FileCache::try_write(std::string_view name, std::string_view content) const
{
    const auto path = path_of(name);
    if (!path) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(*directory_, error);
    if (error) {
        return;
    }
    std::filesystem::path temporary = *path;
    temporary += L".tmp";
    // Exclusive, so a concurrent writer of the same temporary file fails instead of
    // interleaving (06 §3); flushed before the rename publishes it.
    if (win32::write_new_file(temporary, content) != ERROR_SUCCESS) {
        return;
    }
    // A failed replace (e.g. the target is open without delete sharing) leaves the
    // .tmp behind and keeps the old file, like .NET (04 §6).
    ::MoveFileExW(temporary.c_str(), path->c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void FileCache::try_delete(std::string_view name) const
{
    if (const auto path = path_of(name)) {
        ::DeleteFileW(path->c_str()); // a missing file and access errors are ignored (04 §6)
    }
}

} // namespace wizz
