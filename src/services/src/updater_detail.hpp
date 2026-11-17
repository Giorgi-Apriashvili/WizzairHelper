#pragma once
// Helpers shared by the updater's translation units (updater*.cpp).

#include <filesystem>
#include <string_view>

namespace wizz::update::detail {

/// Appends `line` (CR and LF replaced by spaces) and CRLF to the log, rotating it first
/// when it is over kLogRotateBytes. Creates the directory; ignores every I/O error.
void append_raw_line(const std::filesystem::path& log_file, std::string_view line);

/// `<file>.sha256`: where download_verified keeps the hash of a file it verified.
[[nodiscard]] std::filesystem::path kept_hash_path(const std::filesystem::path& file);

} // namespace wizz::update::detail
