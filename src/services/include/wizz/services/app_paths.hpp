#pragma once
// Where the app keeps its files (04 §6, 02 §7, 01 §3.2 and §13, 06 §3):
//   %LOCALAPPDATA%\WizzairHelper\settings.json, errors.log
//   %LOCALAPPDATA%\WizzairHelper\cache\api-url.txt, map.json, fx.json, geocode.json
// Every function takes an optional root override so tests (and an isolated
// --autotest profile, D-30) can supply their own directory.

#include <filesystem>
#include <optional>

namespace wizz::app_paths {

/// `root_override` when given (used as is), otherwise
/// SHGetKnownFolderPath(FOLDERID_LocalAppData) + `\WizzairHelper`. Throws
/// std::system_error when the known folder cannot be resolved.
[[nodiscard]] std::filesystem::path default_root(const std::optional<std::filesystem::path>& root_override = std::nullopt);

/// `<root>\cache`, the FileCache directory.
[[nodiscard]] std::filesystem::path default_cache_dir(
    const std::optional<std::filesystem::path>& root_override = std::nullopt);

/// `<root>\settings.json` (01 §13).
[[nodiscard]] std::filesystem::path settings_file(const std::optional<std::filesystem::path>& root_override = std::nullopt);

/// `<root>\errors.log` (01 §3.2).
[[nodiscard]] std::filesystem::path errors_log(const std::optional<std::filesystem::path>& root_override = std::nullopt);

} // namespace wizz::app_paths
