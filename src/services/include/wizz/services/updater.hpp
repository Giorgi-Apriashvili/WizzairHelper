#pragma once
// Self-updater core (Qt-free): the owner's "Self-updating apps" rules and the
// installer / updater contract (DECISIONS D-40..D-44).
//
//   - Finding the release: GET releases/latest from the GitHub API and pick the
//     assets by their stable names (WizzairHelper-X.Y.Z-win64-setup.exe,
//     WizzairHelper-X.Y.Z-win64.zip, SHA256SUMS.txt).
//   - Checking it: SHA256SUMS.txt is downloaded first, and a file whose SHA-256
//     does not match is deleted, never used.
//   - Downloading it: retries on 5xx, 408, 429 and network errors with a fresh query
//     string on each try (GitHub's edge caches error answers per URL), then the API
//     asset URL with `Accept: application/octet-stream`; every error carries the last
//     HTTP status and every try is logged.
//   - A daily schedule kept in `<data root>\updates\update-state.json` (settings.json
//     has a fixed schema), the log in `<data root>\updates\update.log` and downloads in
//     `<data root>\updates\downloads\` — nothing of the updater lives in the data root
//     itself.
//   - Installed (unins000.exe next to the exe) or portable, and the silent installer
//     command line. Starting a process or quitting the app is the UI's job.
//
// User cancellation is wizz::OperationCancelled (thrown), never an UpdateError.

#include "wizz/core/cancellation.hpp"
#include "wizz/core/http.hpp"
#include "wizz/core/time.hpp"
#include "wizz/services/file_cache.hpp"

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wizz::update {

// --- The contract -------------------------------------------------------------------

/// The GitHub repository the releases come from.
inline constexpr std::string_view kOwner = "Giorgi-Apriashvili";
inline constexpr std::string_view kRepo = "WizzairHelper";

/// `X-GitHub-Api-Version` of every API request.
inline constexpr std::string_view kGitHubApiVersion = "2022-11-28";
/// Accept header of the release query and of the API asset URL.
inline constexpr std::string_view kGitHubJson = "application/vnd.github+json";
inline constexpr std::string_view kOctetStream = "application/octet-stream";

/// Stable asset names: `<prefix><X.Y.Z><suffix>`, plus the checksum file.
inline constexpr std::string_view kAssetPrefix = "WizzairHelper-";
inline constexpr std::string_view kZipSuffix = "-win64.zip";
inline constexpr std::string_view kInstallerSuffix = "-win64-setup.exe";
inline constexpr std::string_view kChecksumsName = "SHA256SUMS.txt";

/// An installed copy has the Inno Setup uninstaller next to WizzairHelper.exe.
inline constexpr std::wstring_view kUninstallerName = L"unins000.exe";
/// The named mutex the app holds while it runs (installer `AppMutex`).
inline constexpr std::wstring_view kAppMutexName = L"WizzairHelper.AppMutex";

/// Files of the updater, all under `app_paths::updates_dir(<data root>)`.
inline constexpr std::string_view kStateFileName = "update-state.json";
inline constexpr std::string_view kLogFileName = "update.log";
inline constexpr std::wstring_view kDownloadsDirName = L"downloads";

// --- Versions ------------------------------------------------------------------------

/// A release version `X.Y.Z` (numeric components, compared numerically in order).
struct Version {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;

    friend constexpr auto operator<=>(const Version&, const Version&) noexcept = default;
    friend constexpr bool operator==(const Version&, const Version&) noexcept = default;

    /// "X.Y.Z".
    [[nodiscard]] std::string to_string() const;
};

/// Exactly `X.Y.Z`: three ASCII decimal components of 1..9 digits, no leading zero
/// (except "0" itself), nothing before or after. nullopt otherwise ("1.2", "1.2.3.4",
/// "01.2.3", "1.2.3-rc1", " 1.2.3").
[[nodiscard]] std::optional<Version> parse_version(std::string_view text) noexcept;

/// A release tag: a lowercase `v` followed by `parse_version` text ("v1.2.3").
[[nodiscard]] std::optional<Version> parse_tag(std::string_view tag) noexcept;

// --- Configuration -------------------------------------------------------------------

/// What the updater needs to know about the running app.
struct UpdateConfig {
    std::string owner{kOwner};
    std::string repo{kRepo};
    Version current_version;
    /// `%LOCALAPPDATA%\WizzairHelper` or the `--data-root` directory; the updater's files
    /// live in its `updates` subdirectory.
    std::filesystem::path data_root;

    /// The app's configuration: `current_version` is WIZZ_VERSION_STRING. Throws
    /// std::invalid_argument when it is not `X.Y.Z`.
    [[nodiscard]] static UpdateConfig for_app(std::string_view current_version, std::filesystem::path data_root);

    /// "WizzairHelper/<X.Y.Z> (update check)", sent with every request.
    [[nodiscard]] std::string user_agent() const;
    /// "https://api.github.com/repos/<owner>/<repo>/releases/latest".
    [[nodiscard]] std::string latest_release_url() const;
    /// "WizzairHelper-<X.Y.Z>-win64-setup.exe".
    [[nodiscard]] static std::string installer_name(const Version& version);
    /// "WizzairHelper-<X.Y.Z>-win64.zip".
    [[nodiscard]] static std::string zip_name(const Version& version);

    /// `<data root>\updates`.
    [[nodiscard]] std::filesystem::path updates_dir() const;
    /// `<data root>\updates\update-state.json`.
    [[nodiscard]] std::filesystem::path state_file() const;
    /// `<data root>\updates\update.log`.
    [[nodiscard]] std::filesystem::path log_file() const;
    /// `<data root>\updates\downloads`, where the app downloads release assets.
    [[nodiscard]] std::filesystem::path download_dir() const;
};

// --- Errors and the log of tries -----------------------------------------------------

/// One HTTP try, as returned to the caller and appended to update.log.
struct TryRecord {
    UtcInstant started_at{};
    std::string method;               ///< "GET"
    std::string url;                  ///< as sent, including the fresh query string
    std::optional<int> status;        ///< the HTTP status, nullopt when no response arrived
    std::string error;                ///< "network error: …", "timeout: …" or "cancelled"; "" with a status
    std::uint64_t bytes = 0;          ///< body bytes received
    std::chrono::milliseconds duration{0};

    friend bool operator==(const TryRecord&, const TryRecord&) = default;
};

/// The update.log line of a try (without the line break):
/// `<started_at ISO UTC> GET <url> -> HTTP 200, 1234 bytes, 56 ms`, or
/// `... -> network error: <message>, 0 bytes, 56 ms`.
[[nodiscard]] std::string format_try_line(const TryRecord& record);

enum class UpdateErrorKind : std::uint8_t {
    network,            ///< DNS, connect, TLS, timeout or a broken transfer; no HTTP status
    http_status,        ///< GitHub answered with a status that is not 2xx
    no_release,         ///< nothing to update to: 404 on releases/latest, a draft or a prerelease
    bad_release,        ///< the release JSON is unreadable, lacks tag_name / html_url, or the tag is not vX.Y.Z
    missing_asset,      ///< the release lacks the requested asset or SHA256SUMS.txt
    bad_checksums,      ///< SHA256SUMS.txt is malformed or has no line for the asset
    checksum_mismatch,  ///< the download's SHA-256 differs from SHA256SUMS.txt; it was deleted
    file_system,        ///< the destination could not be created, written or renamed into
};

struct UpdateError {
    UpdateErrorKind kind = UpdateErrorKind::network;
    /// One English sentence for the log and the UI; it names the HTTP status when there is one.
    std::string message;
    /// The last HTTP status received during the operation; nullopt when none arrived.
    std::optional<int> http_status;
    /// Every try of the operation, in order (also in update.log).
    std::vector<TryRecord> tries;
};

// --- Releases ------------------------------------------------------------------------

struct ReleaseAsset {
    std::string name;
    std::string browser_download_url;  ///< https://github.com/<owner>/<repo>/releases/download/...
    std::string api_url;               ///< https://api.github.com/repos/<owner>/<repo>/releases/assets/<id>
    std::uint64_t size = 0;            ///< bytes as GitHub reports them; 0 when absent

    friend bool operator==(const ReleaseAsset&, const ReleaseAsset&) = default;
};

enum class AssetKind : std::uint8_t { installer, portable_zip };

struct ReleaseInfo {
    std::string tag;       ///< "v1.2.0"
    Version version;       ///< parsed from the tag
    std::string html_url;  ///< the release page
    bool newer = false;    ///< version > UpdateConfig::current_version
    std::optional<ReleaseAsset> installer;
    std::optional<ReleaseAsset> portable_zip;
    std::optional<ReleaseAsset> checksums;

    [[nodiscard]] const std::optional<ReleaseAsset>& asset(AssetKind kind) const noexcept
    {
        return kind == AssetKind::installer ? installer : portable_zip;
    }
};

/// Reads a releases/latest answer (already decoded to UTF-8 text):
/// - not JSON or not an object → bad_release;
/// - `draft` or `prerelease` literally true → no_release (such releases are ignored);
/// - `tag_name` or `html_url` missing or not a string → bad_release; a tag that is not
///   `parse_tag` text → bad_release (never "newer");
/// - `assets`: an element counts only when it is an object whose `name` equals one of
///   the three stable names for the tag's version exactly, whose `state` (when present)
///   is "uploaded" and whose `browser_download_url` and `url` are https URLs; anything
///   else is skipped, so a missing asset is simply nullopt.
[[nodiscard]] std::expected<ReleaseInfo, UpdateError> parse_release(std::string_view json_text,
                                                                    const UpdateConfig& config);

// --- Checksums -----------------------------------------------------------------------

struct ChecksumLine {
    std::string sha256;     ///< 64 lowercase hex digits
    std::string file_name;

    friend bool operator==(const ChecksumLine&, const ChecksumLine&) = default;
};

/// Parses SHA256SUMS.txt: per line 64 hex digits (upper case is lowered), then two
/// spaces or a space and `*`, then the file name (rest of the line, at least one
/// character). LF or CRLF line ends, a leading UTF-8 BOM and blank lines are allowed.
/// Any other line, or two lines naming the same file with different hashes, makes the
/// whole file invalid: the error text names the first bad line ("line 3").
[[nodiscard]] std::expected<std::vector<ChecksumLine>, std::string> parse_sha256sums(std::string_view text);

/// The hash listed for `file_name` (exact, case-sensitive), nullopt when absent.
[[nodiscard]] std::optional<std::string> find_checksum(std::span<const ChecksumLine> lines,
                                                       std::string_view file_name);

/// Lowercase hex SHA-256 (Windows CNG). Throws std::runtime_error when CNG fails.
[[nodiscard]] std::string sha256_hex(std::string_view bytes);
/// Lowercase hex SHA-256 of a file's bytes; nullopt when it cannot be read.
[[nodiscard]] std::optional<std::string> sha256_file(const std::filesystem::path& path);

// --- Daily schedule ------------------------------------------------------------------

/// A check is due when the last successful one is at least this old.
inline constexpr Duration kCheckInterval = std::chrono::hours{24};
/// After a failed check, the next one waits at least this long.
inline constexpr Duration kFailureRetryInterval = std::chrono::hours{1};

/// `update-state.json`: an object with `LastSuccessfulCheckUtc`, `LatestSeenVersion`
/// and `LastFailedCheckUtc` (each omitted when unknown; times `yyyy-MM-ddTHH:mm:ssZ`).
struct UpdateState {
    std::optional<UtcInstant> last_success;
    std::optional<Version> latest_seen;
    std::optional<UtcInstant> last_failure;

    friend bool operator==(const UpdateState&, const UpdateState&) = default;

    /// Any problem — malformed JSON, a non-object root, a value of the wrong type or
    /// format — gives the empty state (a check becomes due). Never throws.
    [[nodiscard]] static UpdateState parse(std::string_view json_text) noexcept;
    /// Indented JSON (two spaces, CRLF), present fields only, in the order above.
    [[nodiscard]] std::string to_json() const;
};

/// True when no check succeeded in the last kCheckInterval (`now - last_success >=
/// 24 h`, or never) and no check failed in the last kFailureRetryInterval. A time in
/// the future (the clock was set back) is not trusted: it does not block a check.
[[nodiscard]] bool is_check_due(const UpdateState& state, UtcInstant now) noexcept;

/// The persisted schedule in `<updates dir>\update-state.json`, written atomically
/// (FileCache). Read and write errors are swallowed: an unreadable file is an empty
/// state, an unwritable one only means the next start checks again.
class UpdateSchedule {
public:
    /// Throws std::invalid_argument for a null clock.
    UpdateSchedule(std::filesystem::path updates_dir, std::shared_ptr<const Clock> clock);

    [[nodiscard]] UpdateState load() const;
    /// `is_check_due(load(), clock.utc_now())`.
    [[nodiscard]] bool is_check_due() const;

    /// A check succeeded now: sets last_success and latest_seen, clears last_failure.
    void record_success(std::optional<Version> latest_seen) const;
    /// A check failed now: sets last_failure, keeps the rest.
    void record_failure() const;
    /// Records a check_latest result: a release or no_release is a success, any other
    /// error a failure.
    void record(const std::expected<ReleaseInfo, UpdateError>& result) const;

private:
    void save(const UpdateState& state) const;

    FileCache store_;
    std::shared_ptr<const Clock> clock_;
};

// --- Network -------------------------------------------------------------------------

/// Tries per asset: on the browser_download_url first, then on the API asset URL.
inline constexpr int kBrowserTries = 3;
inline constexpr int kApiTries = 2;
/// Wait before the n-th retry after a retryable failure (the last value repeats).
inline constexpr std::array<Duration, 4> kRetryDelays{std::chrono::seconds{2}, std::chrono::seconds{4},
                                                      std::chrono::seconds{8}, std::chrono::seconds{16}};
/// Per-try timeouts (whole transfer, redirects included).
inline constexpr std::chrono::milliseconds kCheckTimeout{30'000};
inline constexpr std::chrono::milliseconds kChecksumsTimeout{30'000};
inline constexpr std::chrono::milliseconds kAssetTimeout{600'000};

/// 5xx, 408 and 429 are worth another try; any other status is not.
[[nodiscard]] constexpr bool is_retryable_status(int status) noexcept
{
    return (status >= 500 && status <= 599) || status == 408 || status == 429;
}

/// `url` with `attempt=<n>&t=<unix milliseconds of now>` appended as a query (`?`, or
/// `&` when the URL already has one), so no try reuses a URL whose error GitHub's edge
/// may have cached.
[[nodiscard]] std::string with_fresh_query(std::string_view url, int attempt, UtcInstant now);

/// A file the updater downloaded and verified.
struct DownloadedFile {
    std::filesystem::path path;  ///< `<destination>\<asset name>`
    std::string sha256;          ///< lowercase hex, equal to the SHA256SUMS.txt line
    bool reused = false;         ///< a verified file of that name was already there; nothing was downloaded
    std::vector<TryRecord> tries;
};

/// The network side of the updater. Stateless apart from its configuration and safe
/// to use from any (worker) thread; it blocks, so the UI calls it off its own thread.
class Updater {
public:
    /// `delay` waits between tries (tests inject a recorder). Throws
    /// std::invalid_argument for a null transport or clock or an empty delay.
    Updater(UpdateConfig config, std::shared_ptr<http::Transport> transport, std::shared_ptr<const Clock> clock,
            DelayFunc delay = cancellable_sleep);

    [[nodiscard]] const UpdateConfig& config() const noexcept { return config_; }

    /// One GET of `latest_release_url()` with User-Agent, `Accept:
    /// application/vnd.github+json` and `X-GitHub-Api-Version: 2022-11-28` (kCheckTimeout).
    /// 2xx → `parse_release`; 404 → no_release; another status → http_status; a
    /// transport error or timeout → network. Not retried: the schedule retries a
    /// failed check after kFailureRetryInterval. The try and the outcome are appended
    /// to update.log. Throws OperationCancelled when `stop` fires.
    [[nodiscard]] std::expected<ReleaseInfo, UpdateError> check_latest(std::stop_token stop) const;

    /// The daily check: nullopt without any request when `schedule.is_check_due()` is
    /// false; otherwise check_latest, recorded in the schedule, and its result.
    [[nodiscard]] std::optional<std::expected<ReleaseInfo, UpdateError>> check_if_due(const UpdateSchedule& schedule,
                                                                                     std::stop_token stop) const;

    /// Downloads `release.asset(kind)` into `destination_dir` and verifies it:
    /// 1. SHA256SUMS.txt first (same tries as below, kChecksumsTimeout), parsed; the
    ///    asset's line must be there (bad_checksums otherwise; the asset is not fetched);
    /// 2. a file of the asset's name already in `destination_dir` whose SHA-256 matches
    ///    is kept (`reused`); one that does not match is deleted;
    /// 3. the asset (kAssetTimeout per try): up to kBrowserTries on browser_download_url
    ///    (User-Agent only), then up to kApiTries on the API asset URL (User-Agent,
    ///    `Accept: application/octet-stream`, `X-GitHub-Api-Version`), every try with a
    ///    fresh query (`with_fresh_query`, a counter shared by both files). 5xx, 408, 429,
    ///    transport errors and timeouts are retried after kRetryDelays (cancellable);
    ///    another status ends that URL at once (a browser URL falls back to the API URL);
    /// 4. written to `<name>.tmp` in `destination_dir` (created when missing), hashed
    ///    from disk, deleted on a mismatch (checksum_mismatch), else renamed into place.
    /// missing_asset when the release lacks the asset or SHA256SUMS.txt (no request).
    /// Every try is in the result (or the error) and in update.log. Throws
    /// OperationCancelled when `stop` fires; no .tmp file is left behind.
    [[nodiscard]] std::expected<DownloadedFile, UpdateError> download_verified(
        const ReleaseInfo& release, AssetKind kind, const std::filesystem::path& destination_dir,
        std::stop_token stop) const;

private:
    class Operation;

    UpdateConfig config_;
    std::shared_ptr<http::Transport> transport_;
    std::shared_ptr<const Clock> clock_;
    DelayFunc delay_;
};

/// Appends `<now ISO UTC> <text>` and CRLF to update.log (directory created when
/// missing). Before the append, a log over kLogRotateBytes is moved to
/// `update.log.old` (replacing it). I/O errors are ignored; CR and LF in `text` become
/// spaces.
void append_log_line(const std::filesystem::path& log_file, UtcInstant now, std::string_view text);
inline constexpr std::uintmax_t kLogRotateBytes = 1u << 20;

// --- Installing ----------------------------------------------------------------------

enum class InstallKind : std::uint8_t { installed, portable };

/// `installed` when unins000.exe is a regular file next to `exe_path`, else `portable`.
[[nodiscard]] InstallKind detect_install_kind(const std::filesystem::path& exe_path);
/// The running executable (GetModuleFileNameW). Throws std::system_error on failure.
[[nodiscard]] std::filesystem::path running_executable_path();

/// What an install kind updates with: the installer for an installed copy, the ZIP
/// for a portable one.
[[nodiscard]] constexpr AssetKind asset_kind_for(InstallKind kind) noexcept
{
    return kind == InstallKind::installed ? AssetKind::installer : AssetKind::portable_zip;
}

enum class Relaunch : std::uint8_t { no, yes };

/// The silent update: `<setup.exe> /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
/// /CLOSEAPPLICATIONS [/RELAUNCH]`.
struct InstallerCommand {
    std::filesystem::path program;       ///< the verified setup.exe
    std::vector<std::string> arguments;  ///< the switches, in order

    /// The switches joined by single spaces (ShellExecuteExW lpParameters).
    [[nodiscard]] std::string parameters() const;
    /// `"<program>" <parameters>` (CreateProcessW lpCommandLine); the program is always quoted.
    [[nodiscard]] std::wstring command_line() const;

    friend bool operator==(const InstallerCommand&, const InstallerCommand&) = default;
};

/// The contract's silent installer command; `/RELAUNCH` makes the installer start
/// WizzairHelper.exe again after a successful install.
[[nodiscard]] InstallerCommand silent_install_command(const std::filesystem::path& setup_exe, Relaunch relaunch);

/// Deletes downloaded assets of versions up to `config.current_version` (and their
/// leftover `.tmp` files) from `config.download_dir()`; other files and newer versions
/// stay. Returns how many files were deleted; errors are ignored.
std::size_t remove_stale_downloads(const UpdateConfig& config);

} // namespace wizz::update
