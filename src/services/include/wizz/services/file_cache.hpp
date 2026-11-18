#pragma once
// Small text files in the cache directory (04 §6, 02 §7, 06 §3): api-url.txt,
// map.json, fx.json, geocode.json and fares.json (the reused fares, DECISIONS D-67). Freshness follows each file's last-write time
// against the injected clock; there is no background cleanup. Failures never escape:
// an unreadable or unwritable cache must not prevent an online search.

#include "wizz/core/time.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace wizz {

/// .NET `FileCache`. Thread-safe (stateless apart from the directory). Concurrent
/// writers, also from other app instances: the last complete write wins.
class FileCache {
public:
    /// `directory` nullopt disables the cache (`FileCache.Disabled`): reads return
    /// nullopt, writes and deletes do nothing. The directory is created on first write.
    explicit FileCache(std::optional<std::filesystem::path> directory) : directory_(std::move(directory)) {}

    /// The disabled cache used by tests (05 §1.5).
    [[nodiscard]] static FileCache disabled() { return FileCache{std::nullopt}; }

    [[nodiscard]] const std::optional<std::filesystem::path>& directory() const noexcept { return directory_; }

    /// The text of `<directory>/<name>` (name in UTF-8) as UTF-8, or nullopt when the
    /// cache is disabled, the file is missing or unreadable, or `max_age` is given and
    /// `clock.utc_now() - last write time > max_age` (strictly greater: an age equal to
    /// `max_age` and a write time in the future count as fresh). Decoded like
    /// File.ReadAllText: a UTF-8/16/32 BOM selects the encoding and is stripped,
    /// otherwise UTF-8; ill-formed bytes become U+FFFD.
    [[nodiscard]] std::optional<std::string> try_read(std::string_view name, std::optional<Duration> max_age,
                                                      const Clock& clock) const;

    /// Replaces `<directory>/<name>` with `content` (UTF-8, no BOM): creates the
    /// directory, writes `<name>.tmp` opened exclusively, flushes it and moves it over
    /// the target (MoveFileExW, REPLACE_EXISTING | WRITE_THROUGH). Errors are swallowed;
    /// a leftover `.tmp` is not cleaned up.
    void try_write(std::string_view name, std::string_view content) const;

    /// Deletes `<directory>/<name>`; a missing file and errors are ignored.
    void try_delete(std::string_view name) const;

private:
    [[nodiscard]] std::optional<std::filesystem::path> path_of(std::string_view name) const;

    std::optional<std::filesystem::path> directory_;
};

} // namespace wizz
