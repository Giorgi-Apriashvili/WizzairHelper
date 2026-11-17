// Updater: the release check and the verified download with its retry policy and
// try log (D-41, D-42, D-43).

#include "wizz/services/updater.hpp"

#include "updater_detail.hpp"
#include "win32_util.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <format>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace wizz::update {
namespace {

/// What one try produced: a response, or the reason none arrived.
struct TryOutcome {
    std::optional<http::Response> response;
    std::string error;  ///< "network error: …" or "timeout: …" when there is no response
};

[[nodiscard]] constexpr bool is_success(int status) noexcept
{
    return status >= 200 && status <= 299;
}

[[nodiscard]] std::string plural_tries(int count)
{
    return std::format("{} {}", count, count == 1 ? "try" : "tries");
}

/// Release asset names come from GitHub; never let one leave the destination directory.
[[nodiscard]] bool is_plain_file_name(std::string_view name) noexcept
{
    return !name.empty() && name != "." && name != ".." && name.find_first_of("\\/:") == std::string_view::npos;
}

} // namespace

/// One check or download: the tries so far, the shared attempt counter for fresh query
/// strings, the last HTTP status and the log.
class Updater::Operation {
public:
    Operation(const Updater& owner, std::stop_token stop) : owner_(owner), stop_(std::move(stop)) {}

    [[nodiscard]] int next_attempt() noexcept { return ++attempt_; }

    /// Sends one request and records the try (returned list and update.log). A user
    /// cancel — also a timeout or transport error seen while `stop` is set — is logged
    /// as "cancelled" and thrown as OperationCancelled.
    [[nodiscard]] TryOutcome send(const http::Request& request)
    {
        throw_if_cancelled(stop_);
        const Clock& clock = *owner_.clock_;
        TryRecord record{.started_at = clock.utc_now(), .method = request.method, .url = request.url};
        const Ticks started = clock.monotonic_now();
        TryOutcome outcome;
        try {
            outcome.response = owner_.transport_->send(request, stop_);
        } catch (const OperationCancelled&) {
            cancelled(record, started);
            throw;
        } catch (const http::TimeoutError& error) {
            if (stop_.stop_requested()) {
                cancelled(record, started);
                throw OperationCancelled{};
            }
            outcome.error = std::format("timeout: {}", error.what());
        } catch (const http::TransportError& error) {
            if (stop_.stop_requested()) {
                cancelled(record, started);
                throw OperationCancelled{};
            }
            outcome.error = std::format("network error: {}", error.what());
        }
        if (outcome.response) {
            record.status = outcome.response->status;
            record.bytes = outcome.response->body.size();
            last_status_ = outcome.response->status;
        } else {
            record.error = outcome.error;
        }
        finish(std::move(record), started);
        return outcome;
    }

    /// Appends `<now> <text>` to update.log.
    void log(std::string_view text) const
    {
        append_log_line(owner_.config_.log_file(), owner_.clock_->utc_now(), text);
    }

    /// Logs `message` and builds the error with the last status and every try.
    [[nodiscard]] std::unexpected<UpdateError> fail(UpdateErrorKind kind, std::string message)
    {
        log(message);
        return std::unexpected(
            UpdateError{.kind = kind, .message = std::move(message), .http_status = last_status_, .tries = take_tries()});
    }

    [[nodiscard]] std::vector<TryRecord> take_tries() noexcept { return std::exchange(tries_, {}); }

    /// The asset's bytes through the retry policy (D-42): kBrowserTries on the browser
    /// URL, then kApiTries on the API URL with `Accept: application/octet-stream`.
    [[nodiscard]] std::expected<std::string, UpdateError> fetch(const ReleaseAsset& asset,
                                                                std::chrono::milliseconds timeout)
    {
        struct Source {
            const std::string& url;
            bool api;
            int tries;
        };
        const std::array<Source, 2> sources{Source{asset.browser_download_url, false, kBrowserTries},
                                            Source{asset.api_url, true, kApiTries}};
        int tries = 0;
        int retryable_failures = 0;
        bool wait_before_next = false;
        UpdateErrorKind last_kind = UpdateErrorKind::network;
        std::string last_problem;
        for (const Source& source : sources) {
            for (int i = 0; i < source.tries; ++i) {
                if (wait_before_next) {
                    const auto step = std::min(static_cast<std::size_t>(retryable_failures), kRetryDelays.size());
                    owner_.delay_(kRetryDelays[step - 1], stop_);
                    wait_before_next = false;
                }
                http::Request request;
                request.method = "GET";
                request.url = with_fresh_query(source.url, next_attempt(), owner_.clock_->utc_now());
                request.headers = {{"User-Agent", owner_.config_.user_agent()}};
                if (source.api) {
                    request.headers.emplace_back("Accept", std::string{kOctetStream});
                    request.headers.emplace_back("X-GitHub-Api-Version", std::string{kGitHubApiVersion});
                }
                request.timeout = timeout;
                ++tries;
                TryOutcome outcome = send(request);
                if (outcome.response) {
                    const int status = outcome.response->status;
                    if (is_success(status)) {
                        return std::move(outcome.response->body);
                    }
                    last_kind = UpdateErrorKind::http_status;
                    last_problem = std::format("HTTP {}.", status);
                    if (!is_retryable_status(status)) {
                        break; // this URL will not get better; a browser URL falls back to the API URL
                    }
                } else {
                    last_kind = UpdateErrorKind::network;
                    last_problem = std::move(outcome.error);
                }
                ++retryable_failures;
                wait_before_next = true;
            }
        }
        return fail(last_kind,
                    std::format("Could not download {} after {}: {}", asset.name, plural_tries(tries), last_problem));
    }

private:
    void finish(TryRecord record, Ticks started)
    {
        record.duration = std::chrono::floor<std::chrono::milliseconds>(owner_.clock_->monotonic_now() - started);
        detail::append_raw_line(owner_.config_.log_file(), format_try_line(record));
        tries_.push_back(std::move(record));
    }

    void cancelled(TryRecord& record, Ticks started)
    {
        record.error = "cancelled";
        finish(std::move(record), started);
    }

    const Updater& owner_;
    std::stop_token stop_;
    std::vector<TryRecord> tries_;
    std::optional<int> last_status_;
    int attempt_ = 0;
};

Updater::Updater(UpdateConfig config, std::shared_ptr<http::Transport> transport, std::shared_ptr<const Clock> clock,
                 DelayFunc delay)
    : config_(std::move(config)), transport_(std::move(transport)), clock_(std::move(clock)), delay_(std::move(delay))
{
    if (!transport_ || !clock_ || !delay_) {
        throw std::invalid_argument("Updater needs a transport, a clock and a delay");
    }
}

std::expected<ReleaseInfo, UpdateError> Updater::check_latest(std::stop_token stop) const
{
    Operation operation{*this, std::move(stop)};
    try {
        http::Request request;
        request.method = "GET";
        request.url = config_.latest_release_url();
        request.headers = {{"User-Agent", config_.user_agent()},
                           {"Accept", std::string{kGitHubJson}},
                           {"X-GitHub-Api-Version", std::string{kGitHubApiVersion}}};
        request.timeout = kCheckTimeout;
        const TryOutcome outcome = operation.send(request);
        if (!outcome.response) {
            return operation.fail(UpdateErrorKind::network, std::format("Update check failed: {}", outcome.error));
        }
        const int status = outcome.response->status;
        if (status == 404) {
            return operation.fail(UpdateErrorKind::no_release,
                                  "No release is published yet (GitHub answered HTTP 404).");
        }
        if (!is_success(status)) {
            return operation.fail(UpdateErrorKind::http_status,
                                  std::format("Update check failed: GitHub answered HTTP {}.", status));
        }
        auto release = parse_release(http::decode_body_text(*outcome.response), config_);
        if (!release) {
            return operation.fail(release.error().kind, std::move(release.error().message));
        }
        operation.log(std::format("Update check: latest release {}, running {}.", release->tag,
                                  config_.current_version.to_string()));
        return release;
    } catch (const OperationCancelled&) {
        operation.log("Cancelled.");
        throw;
    }
}

std::optional<std::expected<ReleaseInfo, UpdateError>> Updater::check_if_due(const UpdateSchedule& schedule,
                                                                            const CheckPolicy& policy,
                                                                            std::stop_token stop) const
{
    if (!schedule.is_check_due(policy)) {
        return std::nullopt;
    }
    return check_now(schedule, std::move(stop));
}

std::expected<ReleaseInfo, UpdateError> Updater::check_now(const UpdateSchedule& schedule, std::stop_token stop) const
{
    auto result = check_latest(std::move(stop)); // a cancelled check throws and records nothing
    schedule.record(result);
    return result;
}

std::expected<DownloadedFile, UpdateError> Updater::download_verified(const ReleaseInfo& release, AssetKind kind,
                                                                      const std::filesystem::path& destination_dir,
                                                                      std::stop_token stop) const
{
    Operation operation{*this, std::move(stop)};
    try {
        const auto& asset = release.asset(kind);
        if (!asset) {
            const std::string name = kind == AssetKind::installer ? UpdateConfig::installer_name(release.version)
                                                                   : UpdateConfig::zip_name(release.version);
            return operation.fail(UpdateErrorKind::missing_asset, std::format("Release {} has no {}.", release.tag, name));
        }
        if (!release.checksums) {
            return operation.fail(UpdateErrorKind::missing_asset,
                                  std::format("Release {} has no {}.", release.tag, kChecksumsName));
        }
        if (!is_plain_file_name(asset->name)) {
            return operation.fail(UpdateErrorKind::bad_release,
                                  std::format("Release {} names an asset \"{}\" that is not a file name.", release.tag,
                                              asset->name));
        }

        // 1. SHA256SUMS.txt first: nothing is fetched that could not be checked.
        auto sums = operation.fetch(*release.checksums, kChecksumsTimeout);
        if (!sums) {
            return std::unexpected(std::move(sums.error()));
        }
        const auto lines = parse_sha256sums(*sums);
        if (!lines) {
            return operation.fail(UpdateErrorKind::bad_checksums,
                                  std::format("{} of {} is malformed ({}).", kChecksumsName, release.tag, lines.error()));
        }
        const auto expected_hash = find_checksum(*lines, asset->name);
        if (!expected_hash) {
            return operation.fail(UpdateErrorKind::bad_checksums, std::format("{} of {} has no line for {}.",
                                                                              kChecksumsName, release.tag, asset->name));
        }

        // 2. An earlier verified download of the same file is kept; anything else there goes.
        const std::filesystem::path target = destination_dir / win32::to_wide(asset->name);
        const std::filesystem::path kept_hash = detail::kept_hash_path(target);
        // The hash next to a verified file, so a later session can hash it again without a
        // request (find_verified_download); without it the file is only never offered so.
        const auto keep_hash = [&](const std::string& sha256) {
            if (const DWORD code = win32::write_new_file(kept_hash, std::format("{}  {}\n", sha256, asset->name));
                code != ERROR_SUCCESS) {
                ::DeleteFileW(kept_hash.c_str());
                operation.log(std::format("Could not keep the hash of {}: {}", asset->name, win32::error_text(code)));
            }
        };
        std::error_code error;
        if (std::filesystem::is_regular_file(target, error)) {
            if (sha256_file(target) == *expected_hash) {
                keep_hash(*expected_hash);
                operation.log(std::format("Kept {}, already downloaded and verified (SHA-256 {}).", asset->name,
                                          *expected_hash));
                return DownloadedFile{
                    .path = target, .sha256 = *expected_hash, .reused = true, .tries = operation.take_tries()};
            }
            std::filesystem::remove(target, error);
        }
        std::filesystem::remove(kept_hash, error);  // it vouched for bytes that are gone

        // 3. The asset itself.
        auto body = operation.fetch(*asset, kAssetTimeout);
        if (!body) {
            return std::unexpected(std::move(body.error()));
        }

        // 4. Written next to its final name, checked on disk, renamed into place.
        const auto save_failed = [&](DWORD code) {
            return operation.fail(UpdateErrorKind::file_system,
                                  std::format("Could not save {} in {}: {}", asset->name,
                                              win32::to_utf8(destination_dir.wstring()), win32::error_text(code)));
        };
        std::filesystem::create_directories(destination_dir, error);
        if (error) {
            return save_failed(static_cast<DWORD>(error.value()));
        }
        if (!std::filesystem::is_directory(destination_dir, error)) {
            return save_failed(ERROR_DIRECTORY);
        }
        std::filesystem::path temporary = target;
        temporary += L".tmp";
        const auto discard_temporary = [&] { ::DeleteFileW(temporary.c_str()); };
        if (const DWORD code = win32::write_new_file(temporary, *body); code != ERROR_SUCCESS) {
            discard_temporary();
            return save_failed(code);
        }
        const auto actual_hash = sha256_file(temporary);
        if (!actual_hash) {
            const DWORD last = ::GetLastError();
            const DWORD code = last != ERROR_SUCCESS ? last : ERROR_READ_FAULT;
            discard_temporary();
            return save_failed(code);
        }
        if (*actual_hash != *expected_hash) {
            discard_temporary();
            return operation.fail(UpdateErrorKind::checksum_mismatch,
                                  std::format("{} does not match {} (expected {}, got {}); the file was deleted.",
                                              asset->name, kChecksumsName, *expected_hash, *actual_hash));
        }
        if (::MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
            FALSE) {
            const DWORD code = ::GetLastError();
            discard_temporary();
            return save_failed(code);
        }
        operation.log(std::format("Verified {} (SHA-256 {}).", asset->name, *actual_hash));
        keep_hash(*actual_hash);
        return DownloadedFile{.path = target, .sha256 = *actual_hash, .reused = false, .tries = operation.take_tries()};
    } catch (const OperationCancelled&) {
        operation.log("Cancelled.");
        throw;
    }
}

} // namespace wizz::update
