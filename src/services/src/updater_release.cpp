// Updater: versions, configuration, the releases/latest answer, SHA256SUMS.txt and the
// texts of the try log (D-41, D-43).

#include "wizz/services/updater.hpp"

#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/json.hpp"
#include "wizz/services/app_paths.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace wizz::update {
namespace {

/// 1..9 ASCII digits without a leading zero (except "0").
[[nodiscard]] std::optional<std::uint32_t> parse_component(std::string_view text) noexcept
{
    if (text.empty() || text.size() > 9 || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return value;
}

[[nodiscard]] bool is_https(std::string_view url) noexcept
{
    return url.starts_with("https://") && url.size() > 8;
}

[[nodiscard]] constexpr bool is_hex_digit(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] constexpr char ascii_lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] std::unexpected<UpdateError> release_error(UpdateErrorKind kind, std::string message)
{
    return std::unexpected(UpdateError{.kind = kind, .message = std::move(message), .http_status = std::nullopt, .tries = {}});
}

[[nodiscard]] std::unexpected<UpdateError> unreadable(std::string_view detail)
{
    return release_error(UpdateErrorKind::bad_release, std::format("The latest release could not be read: {}.", detail));
}

/// `size` as a non-negative integer, 0 when absent or not an integer.
[[nodiscard]] std::uint64_t read_size(const json::Value& asset)
{
    const json::Value* size = asset.find("size");
    if (size == nullptr || !size->is_number()) {
        return 0;
    }
    const std::string_view lexeme = size->number_lexeme();
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value);
    return error == std::errc{} && end == lexeme.data() + lexeme.size() ? value : 0;
}

/// An element of `assets` that may be used: an object with a name, state "uploaded"
/// (when present) and https download and API URLs.
[[nodiscard]] std::optional<ReleaseAsset> read_asset(const json::Value& element)
{
    if (!element.is_object()) {
        return std::nullopt;
    }
    const auto name = element.get_string("name");
    const auto browser = element.get_string("browser_download_url");
    const auto api = element.get_string("url");
    if (!name || !browser || !api || !is_https(*browser) || !is_https(*api)) {
        return std::nullopt;
    }
    if (const json::Value* state = element.find("state");
        state != nullptr && (!state->is_string() || state->as_string() != "uploaded")) {
        return std::nullopt;
    }
    return ReleaseAsset{.name = std::string{*name},
                        .browser_download_url = std::string{*browser},
                        .api_url = std::string{*api},
                        .size = read_size(element)};
}

[[nodiscard]] bool is_literally_true(const json::Value* value) noexcept
{
    return value != nullptr && value->is_true();
}

[[nodiscard]] std::expected<ReleaseInfo, UpdateError> read_release(const json::Value& root, const UpdateConfig& config)
{
    if (!root.is_object()) {
        return unreadable("it is not a JSON object");
    }
    // Such releases are ignored before anything else is looked at (their tags may well be
    // "v1.3.0-rc1"); GitHub's releases/latest should never return them anyway.
    if (is_literally_true(root.find("draft"))) {
        return release_error(UpdateErrorKind::no_release, "The latest release is a draft.");
    }
    if (is_literally_true(root.find("prerelease"))) {
        return release_error(UpdateErrorKind::no_release, "The latest release is a prerelease.");
    }
    const auto tag = root.get_string("tag_name");
    if (!tag) {
        return unreadable("tag_name is missing");
    }
    const auto version = parse_tag(*tag);
    if (!version) {
        return unreadable(std::format("the tag \"{}\" is not vX.Y.Z", *tag));
    }
    const auto page = root.get_string("html_url");
    if (!page) {
        return unreadable("html_url is missing");
    }
    if (!is_https(*page)) {
        return unreadable("html_url is not an https URL");
    }

    ReleaseInfo info{.tag = std::string{*tag},
                     .version = *version,
                     .html_url = std::string{*page},
                     .newer = *version > config.current_version,
                     .installer = std::nullopt,
                     .portable_zip = std::nullopt,
                     .checksums = std::nullopt};
    const std::string installer = UpdateConfig::installer_name(*version);
    const std::string zip = UpdateConfig::zip_name(*version);
    if (const json::Value* assets = root.find("assets"); assets != nullptr && assets->is_array()) {
        for (const json::Value& element : assets->as_array()) {
            auto asset = read_asset(element);
            if (!asset) {
                continue;
            }
            if (asset->name == installer) {
                info.installer = std::move(asset);
            } else if (asset->name == zip) {
                info.portable_zip = std::move(asset);
            } else if (asset->name == kChecksumsName) {
                info.checksums = std::move(asset);
            }
        }
    }
    return info;
}

} // namespace

// --- Versions ------------------------------------------------------------------------

std::string Version::to_string() const
{
    return std::format("{}.{}.{}", major, minor, patch);
}

std::optional<Version> parse_version(std::string_view text) noexcept
{
    const auto first = text.find('.');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const auto second = text.find('.', first + 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    const auto major = parse_component(text.substr(0, first));
    const auto minor = parse_component(text.substr(first + 1, second - first - 1));
    const auto patch = parse_component(text.substr(second + 1)); // a third '.' is not a digit
    if (!major || !minor || !patch) {
        return std::nullopt;
    }
    return Version{.major = *major, .minor = *minor, .patch = *patch};
}

std::optional<Version> parse_tag(std::string_view tag) noexcept
{
    if (!tag.starts_with('v')) {
        return std::nullopt;
    }
    return parse_version(tag.substr(1));
}

// --- Configuration -------------------------------------------------------------------

UpdateConfig UpdateConfig::for_app(std::string_view current_version, std::filesystem::path data_root)
{
    const auto version = parse_version(current_version);
    if (!version) {
        throw std::invalid_argument(std::format("The app version \"{}\" is not X.Y.Z.", current_version));
    }
    UpdateConfig config;
    config.current_version = *version;
    config.data_root = std::move(data_root);
    return config;
}

std::string UpdateConfig::user_agent() const
{
    return std::format("WizzairHelper/{} (update check)", current_version.to_string());
}

std::string UpdateConfig::latest_release_url() const
{
    return std::format("https://api.github.com/repos/{}/{}/releases/latest", owner, repo);
}

std::string UpdateConfig::release_page_url(const Version& version) const
{
    return std::format("https://github.com/{}/{}/releases/tag/v{}", owner, repo, version.to_string());
}

std::string UpdateConfig::installer_name(const Version& version)
{
    return std::format("{}{}{}", kAssetPrefix, version.to_string(), kInstallerSuffix);
}

std::string UpdateConfig::zip_name(const Version& version)
{
    return std::format("{}{}{}", kAssetPrefix, version.to_string(), kZipSuffix);
}

std::filesystem::path UpdateConfig::updates_dir() const
{
    return app_paths::updates_dir(data_root);
}

std::filesystem::path UpdateConfig::state_file() const
{
    return updates_dir() / kStateFileName;
}

std::filesystem::path UpdateConfig::log_file() const
{
    return updates_dir() / kLogFileName;
}

std::filesystem::path UpdateConfig::download_dir() const
{
    return updates_dir() / kDownloadsDirName;
}

// --- Releases ------------------------------------------------------------------------

std::expected<ReleaseInfo, UpdateError> parse_release(std::string_view json_text, const UpdateConfig& config)
{
    try {
        return read_release(json::parse(json_text), config);
    } catch (const json::Error&) {
        // Malformed text, or a string holding an unpaired surrogate escape (D-28).
        return unreadable("it is not JSON");
    }
}

// --- Checksums -----------------------------------------------------------------------

std::expected<std::vector<ChecksumLine>, std::string> parse_sha256sums(std::string_view text)
{
    constexpr std::size_t kHexDigits = 64;
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    std::vector<ChecksumLine> lines;
    int number = 0;
    while (!text.empty()) {
        ++number;
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        const auto bad_line = [number] { return std::unexpected(std::format("line {}", number)); };
        if (line.size() <= kHexDigits + 2 || !std::ranges::all_of(line.substr(0, kHexDigits), is_hex_digit)) {
            return bad_line();
        }
        const std::string_view separator = line.substr(kHexDigits, 2);
        if (separator != "  " && separator != " *") {
            return bad_line();
        }
        ChecksumLine entry{.sha256 = std::string{line.substr(0, kHexDigits)},
                           .file_name = std::string{line.substr(kHexDigits + 2)}};
        std::ranges::transform(entry.sha256, entry.sha256.begin(), ascii_lower);
        const auto same_file = std::ranges::find(lines, entry.file_name, &ChecksumLine::file_name);
        if (same_file != lines.end()) {
            if (same_file->sha256 != entry.sha256) {
                return bad_line();
            }
            continue;
        }
        lines.push_back(std::move(entry));
    }
    return lines;
}

std::optional<std::string> find_checksum(std::span<const ChecksumLine> lines, std::string_view file_name)
{
    const auto found = std::ranges::find_if(lines, [&](const ChecksumLine& line) { return line.file_name == file_name; });
    return found == lines.end() ? std::nullopt : std::optional<std::string>{found->sha256};
}

// --- Network texts -------------------------------------------------------------------

std::string format_try_line(const TryRecord& record)
{
    const std::string outcome = record.status ? std::format("HTTP {}", *record.status) : record.error;
    return std::format("{} {} {} -> {}, {} bytes, {} ms", format_iso_utc(record.started_at), record.method, record.url,
                       outcome, record.bytes, record.duration.count());
}

std::string with_fresh_query(std::string_view url, int attempt, UtcInstant now)
{
    const auto unix_ms = std::chrono::floor<std::chrono::milliseconds>(now).time_since_epoch().count();
    return std::format("{}{}attempt={}&t={}", url, url.contains('?') ? '&' : '?', attempt, unix_ms);
}

} // namespace wizz::update
