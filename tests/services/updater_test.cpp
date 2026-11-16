// Self-updater core (DECISIONS D-40..D-44): versions, the releases/latest answer,
// SHA256SUMS.txt, the verified download with its retry policy and log, the daily
// schedule, install-kind detection and the silent installer command line. Fake
// transport, fixed clock and recording delay only; files live in temporary directories.

#include "wizz/services/updater.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/services/app_paths.hpp"

#include "support/fake_transport.hpp"
#include "support/fixtures.hpp"
#include "support/recording_delay.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace http = wizz::http;
namespace update = wizz::update;
using namespace std::chrono_literals;

using update::AssetKind;
using update::InstallKind;
using update::ReleaseInfo;
using update::UpdateConfig;
using update::UpdateErrorKind;
using update::UpdateState;
using update::Version;
using wizz::Duration;
using wizz::FixedClock;
using wizz::test::FakeTransport;
using wizz::test::RecordedRequest;
using wizz::test::RecordingDelay;
using wizz::test::Responder;
using wizz::test::ScriptedResponse;

// --- expected values, spelled out independently of the implementation ----------------

constexpr std::string_view kLatestUrl = "https://api.github.com/repos/Giorgi-Apriashvili/WizzairHelper/releases/latest";
constexpr std::string_view kUserAgent = "WizzairHelper/1.0.0 (update check)";
constexpr std::string_view kDownloadBase = "https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/download/v1.2.0/";
constexpr std::string_view kApiAssetBase = "https://api.github.com/repos/Giorgi-Apriashvili/WizzairHelper/releases/assets/";
constexpr std::string_view kReleasePage = "https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v1.2.0";

constexpr std::string_view kInstaller = "WizzairHelper-1.2.0-win64-setup.exe";
constexpr std::string_view kZip = "WizzairHelper-1.2.0-win64.zip";
constexpr std::string_view kSums = "SHA256SUMS.txt";

// API asset ids of the standard release.
constexpr int kInstallerId = 101;
constexpr int kZipId = 102;
constexpr int kSumsId = 103;

/// 2026-11-02T10:00:00Z (FixedClock default) in Unix milliseconds.
constexpr std::string_view kNowMs = "1793613600000";
constexpr std::string_view kNowIso = "2026-11-02T10:00:00Z";

const std::string kInstallerBytes = "MZ installer bytes \x01\x02\x03 of version 1.2.0";
const std::string kZipBytes = "PK\x03\x04 portable zip of 1.2.0";

[[nodiscard]] std::string browser_url(std::string_view name)
{
    return std::string{kDownloadBase} + std::string{name};
}

[[nodiscard]] std::string api_url(int id)
{
    return std::format("{}{}", kApiAssetBase, id);
}

[[nodiscard]] std::string fresh(std::string_view url, int attempt)
{
    return std::format("{}?attempt={}&t={}", url, attempt, kNowMs);
}

/// A fresh directory under %TEMP%, removed with everything in it afterwards.
class TempDirectory {
public:
    TempDirectory()
    {
        std::random_device entropy;
        path_ = fs::temp_directory_path() / std::format("wizzairhelper-updater-{:08x}{:08x}", entropy(), entropy());
        fs::create_directories(path_);
    }
    ~TempDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

[[nodiscard]] std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void write_file(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::vector<std::string> split_lines(std::string_view text)
{
    std::vector<std::string> lines;
    while (!text.empty()) {
        const auto end = text.find("\r\n");
        if (end == std::string_view::npos) {
            lines.emplace_back(text);
            break;
        }
        lines.emplace_back(text.substr(0, end));
        text.remove_prefix(end + 2);
    }
    return lines;
}

template <typename T>
[[nodiscard]] std::string describe(const std::expected<T, update::UpdateError>& result)
{
    return result ? std::string{"success"} : result.error().message;
}

// --- release JSON -----------------------------------------------------------------------

[[nodiscard]] std::string asset_json(std::string_view name, int id, std::string_view state = "uploaded",
                                     std::string_view download_base = kDownloadBase)
{
    return std::format(R"({{"url":"{}{}","id":{},"name":"{}","label":"","state":"{}","size":{},)"
                       R"("download_count":0,"browser_download_url":"{}{}"}})",
                       kApiAssetBase, id, id, name, state, 1000 + id, download_base, name);
}

struct ReleaseJson {
    std::string tag = "v1.2.0";
    bool draft = false;
    bool prerelease = false;
    std::vector<std::string> assets{asset_json(kInstaller, kInstallerId), asset_json(kZip, kZipId),
                                    asset_json(kSums, kSumsId)};

    [[nodiscard]] std::string text() const
    {
        std::string list;
        for (const auto& asset : assets) {
            list += (list.empty() ? "" : ",") + asset;
        }
        return std::format(R"({{"url":"https://api.github.com/repos/Giorgi-Apriashvili/WizzairHelper/releases/1",)"
                           R"("html_url":"https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/{}",)"
                           R"("id":1,"tag_name":"{}","target_commitish":"main","name":"WizzairHelper {}",)"
                           R"("draft":{},"prerelease":{},"assets":[{}],"body":"Notes"}})",
                           tag, tag, tag, draft, prerelease, list);
    }
};

[[nodiscard]] UpdateConfig config_for(const fs::path& data_root, std::string_view version = "1.0.0")
{
    return UpdateConfig::for_app(version, data_root);
}

[[nodiscard]] ReleaseInfo standard_release(const UpdateConfig& config)
{
    auto parsed = update::parse_release(ReleaseJson{}.text(), config);
    if (!parsed) {
        throw std::logic_error("the standard release must parse: " + parsed.error().message);
    }
    return *std::move(parsed);
}

[[nodiscard]] std::string sums_text(std::string_view installer_bytes = kInstallerBytes,
                                    std::string_view zip_bytes = kZipBytes)
{
    return std::format("{}  {}\n{}  {}\n", wizz::test::sha256_hex(zip_bytes), kZip,
                       wizz::test::sha256_hex(installer_bytes), kInstaller);
}

[[nodiscard]] ScriptedResponse bytes_response(std::string body, int status = 200)
{
    return ScriptedResponse{.response = http::Response{.status = status,
                                                       .headers = {},
                                                       .content_type = "application/octet-stream",
                                                       .body = std::move(body)},
                            .body_error = std::nullopt};
}

[[nodiscard]] ScriptedResponse status_response(int status)
{
    return bytes_response("", status);
}

/// Everything an Updater needs, wired to fakes: FixedClock at 2026-11-02T10:00:00Z, a
/// recording delay and a data root in a temporary directory.
class Harness {
public:
    explicit Harness(Responder responder, RecordingDelay::Hook on_delay = {}, std::string_view version = "1.0.0")
        : clock(std::make_shared<FixedClock>()),
          delay(std::move(on_delay)),
          transport(std::make_shared<FakeTransport>(std::move(responder))),
          config(config_for(temp.path() / "data", version)),
          updater(config, transport, clock, delay.delay_func())
    {
    }

    [[nodiscard]] fs::path downloads() const { return config.download_dir(); }
    [[nodiscard]] std::string log() const { return read_file(config.log_file()); }
    [[nodiscard]] std::vector<std::string> log_lines() const { return split_lines(log()); }

    TempDirectory temp;
    std::shared_ptr<FixedClock> clock;
    RecordingDelay delay;
    std::shared_ptr<FakeTransport> transport;
    UpdateConfig config;
    update::Updater updater;
};

/// Serves SHA256SUMS.txt from the browser URL and hands every asset request to `asset`.
[[nodiscard]] Responder sums_then(std::function<ScriptedResponse(const RecordedRequest&)> asset,
                                  std::string sums = sums_text())
{
    return [asset = std::move(asset), sums = std::move(sums)](const RecordedRequest& request) {
        if (request.url.contains(kSums)) {
            return bytes_response(sums);
        }
        return asset(request);
    };
}

// =====================================================================================
// Versions
// =====================================================================================

TEST(UpdaterVersion, Parses_numeric_X_Y_Z_only)
{
    EXPECT_EQ(update::parse_version("1.2.3"), (Version{1, 2, 3}));
    EXPECT_EQ(update::parse_version("0.0.0"), (Version{0, 0, 0}));
    EXPECT_EQ(update::parse_version("10.20.300"), (Version{10, 20, 300}));
    EXPECT_EQ(update::parse_version("999999999.0.1"), (Version{999'999'999, 0, 1}));

    for (const std::string_view bad : {"", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "1.2.3-rc1",
                                       "1.2.3+build", " 1.2.3", "1.2.3 ", "+1.2.3", "-1.2.3", "1..3", "1.2.",
                                       ".1.2", "a.b.c", "1.2.x", "1234567890.0.0", "v1.2.3", "１.2.3"}) {
        EXPECT_EQ(update::parse_version(bad), std::nullopt) << bad;
    }
    EXPECT_EQ((Version{1, 2, 3}).to_string(), "1.2.3");
}

TEST(UpdaterVersion, Tags_are_a_lowercase_v_and_X_Y_Z)
{
    EXPECT_EQ(update::parse_tag("v1.2.3"), (Version{1, 2, 3}));
    EXPECT_EQ(update::parse_tag("v0.10.0"), (Version{0, 10, 0}));
    for (const std::string_view bad : {"1.2.3", "V1.2.3", "v1.2", "v1.2.3-beta", "vv1.2.3", "v", "", " v1.2.3",
                                       "release-1.2.3", "v01.2.3"}) {
        EXPECT_EQ(update::parse_tag(bad), std::nullopt) << bad;
    }
}

TEST(UpdaterVersion, Compares_numerically_component_by_component)
{
    EXPECT_GT((Version{1, 10, 0}), (Version{1, 9, 9}));  // not lexical: "1.10" > "1.9"
    EXPECT_GT((Version{2, 0, 0}), (Version{1, 99, 99}));
    EXPECT_GT((Version{1, 0, 1}), (Version{1, 0, 0}));
    EXPECT_LT((Version{0, 9, 0}), (Version{1, 0, 0}));
    EXPECT_EQ((Version{1, 2, 3}), (Version{1, 2, 3}));
    EXPECT_FALSE((Version{1, 2, 3}) > (Version{1, 2, 3}));
}

// =====================================================================================
// Configuration and files
// =====================================================================================

TEST(UpdaterConfig, App_configuration_names_the_repository_user_agent_and_the_updates_folder)
{
    const fs::path root = L"C:\\Data\\WizzairHelper";
    const UpdateConfig config = UpdateConfig::for_app("1.0.0", root);

    EXPECT_EQ(config.owner, "Giorgi-Apriashvili");
    EXPECT_EQ(config.repo, "WizzairHelper");
    EXPECT_EQ(config.current_version, (Version{1, 0, 0}));
    EXPECT_EQ(config.user_agent(), kUserAgent);
    EXPECT_EQ(config.latest_release_url(), kLatestUrl);
    EXPECT_EQ(UpdateConfig::installer_name(Version{1, 2, 0}), kInstaller);
    EXPECT_EQ(UpdateConfig::zip_name(Version{1, 2, 0}), kZip);

    // Nothing of the updater lives in the data root itself.
    EXPECT_EQ(wizz::app_paths::updates_dir(root), root / L"updates");
    EXPECT_EQ(config.updates_dir(), root / L"updates");
    EXPECT_EQ(config.state_file(), root / L"updates" / L"update-state.json");
    EXPECT_EQ(config.log_file(), root / L"updates" / L"update.log");
    EXPECT_EQ(config.download_dir(), root / L"updates" / L"downloads");
}

TEST(UpdaterConfig, Rejects_a_malformed_app_version)
{
    EXPECT_THROW((void)UpdateConfig::for_app("1.0", "C:\\Data"), std::invalid_argument);
    EXPECT_THROW((void)UpdateConfig::for_app("v1.0.0", "C:\\Data"), std::invalid_argument);
    EXPECT_THROW((void)UpdateConfig::for_app("", "C:\\Data"), std::invalid_argument);
}

// =====================================================================================
// The releases/latest answer
// =====================================================================================

TEST(UpdaterRelease, Parses_tag_page_and_the_three_assets_with_both_urls)
{
    const auto config = config_for("C:\\Data");
    const auto release = update::parse_release(ReleaseJson{}.text(), config);
    ASSERT_TRUE(release.has_value()) << describe(release);

    EXPECT_EQ(release->tag, "v1.2.0");
    EXPECT_EQ(release->version, (Version{1, 2, 0}));
    EXPECT_EQ(release->html_url, kReleasePage);
    EXPECT_TRUE(release->newer);

    ASSERT_TRUE(release->installer.has_value());
    EXPECT_EQ(*release->installer, (update::ReleaseAsset{.name = std::string{kInstaller},
                                                         .browser_download_url = browser_url(kInstaller),
                                                         .api_url = api_url(kInstallerId),
                                                         .size = 1101}));
    ASSERT_TRUE(release->portable_zip.has_value());
    EXPECT_EQ(release->portable_zip->name, kZip);
    EXPECT_EQ(release->portable_zip->browser_download_url, browser_url(kZip));
    EXPECT_EQ(release->portable_zip->api_url, api_url(kZipId));
    ASSERT_TRUE(release->checksums.has_value());
    EXPECT_EQ(release->checksums->name, kSums);
    EXPECT_EQ(release->checksums->api_url, api_url(kSumsId));

    EXPECT_EQ(&release->asset(AssetKind::installer), &release->installer);
    EXPECT_EQ(&release->asset(AssetKind::portable_zip), &release->portable_zip);
}

TEST(UpdaterRelease, Newer_only_when_the_release_version_is_greater)
{
    const auto json = ReleaseJson{}.text();
    EXPECT_TRUE(update::parse_release(json, config_for("C:\\Data", "1.1.9"))->newer);
    EXPECT_TRUE(update::parse_release(json, config_for("C:\\Data", "0.99.99"))->newer);
    EXPECT_FALSE(update::parse_release(json, config_for("C:\\Data", "1.2.0"))->newer);
    EXPECT_FALSE(update::parse_release(json, config_for("C:\\Data", "1.10.0"))->newer);
    EXPECT_FALSE(update::parse_release(json, config_for("C:\\Data", "2.0.0"))->newer);
}

TEST(UpdaterRelease, Assets_that_do_not_qualify_are_missing)
{
    const auto config = config_for("C:\\Data");

    ReleaseJson only_zip;
    only_zip.assets = {asset_json(kZip, kZipId)};
    const auto zip_release = update::parse_release(only_zip.text(), config);
    ASSERT_TRUE(zip_release.has_value()) << describe(zip_release);
    EXPECT_FALSE(zip_release->installer.has_value());
    EXPECT_TRUE(zip_release->portable_zip.has_value());
    EXPECT_FALSE(zip_release->checksums.has_value());

    ReleaseJson odd;
    odd.assets = {
        asset_json("WizzairHelper-1.1.0-win64-setup.exe", 1),   // another version's name
        asset_json("wizzairhelper-1.2.0-win64.zip", 2),         // names are case-sensitive
        asset_json(kSums, 3, "open"),                           // still uploading
        asset_json(kInstaller, 4, "uploaded", "http://github.com/x/"),  // not https
        R"("WizzairHelper-1.2.0-win64.zip")",                   // not an object
        R"({"name":"SHA256SUMS.txt","url":"https://api.github.com/x/5"})",  // no browser URL
    };
    const auto odd_release = update::parse_release(odd.text(), config);
    ASSERT_TRUE(odd_release.has_value()) << describe(odd_release);
    EXPECT_FALSE(odd_release->installer.has_value());
    EXPECT_FALSE(odd_release->portable_zip.has_value());
    EXPECT_FALSE(odd_release->checksums.has_value());

    ReleaseJson none;
    none.assets.clear();
    const auto empty = update::parse_release(none.text(), config);
    ASSERT_TRUE(empty.has_value());
    EXPECT_FALSE(empty->installer || empty->portable_zip || empty->checksums);

    const auto no_assets_member = update::parse_release(
        R"({"tag_name":"v1.2.0","html_url":"https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v1.2.0"})",
        config);
    ASSERT_TRUE(no_assets_member.has_value()) << describe(no_assets_member);
    EXPECT_FALSE(no_assets_member->checksums.has_value());
}

TEST(UpdaterRelease, Drafts_and_prereleases_are_ignored)
{
    const auto config = config_for("C:\\Data");
    ReleaseJson draft;
    draft.draft = true;
    const auto d = update::parse_release(draft.text(), config);
    ASSERT_FALSE(d.has_value());
    EXPECT_EQ(d.error().kind, UpdateErrorKind::no_release);
    EXPECT_EQ(d.error().message, "The latest release is a draft.");

    ReleaseJson pre;
    pre.prerelease = true;
    pre.tag = "v1.3.0-rc1";  // ignored before its tag is even looked at
    const auto p = update::parse_release(pre.text(), config);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error().kind, UpdateErrorKind::no_release);
    EXPECT_EQ(p.error().message, "The latest release is a prerelease.");
}

TEST(UpdaterRelease, A_malformed_tag_is_an_error_not_newer)
{
    const auto config = config_for("C:\\Data");
    for (const std::string_view tag : {"v1.3", "1.3.0", "latest", "v1.3.0-beta", "v99999999999.0.0"}) {
        ReleaseJson json;
        json.tag = std::string{tag};
        const auto release = update::parse_release(json.text(), config);
        ASSERT_FALSE(release.has_value()) << tag;
        EXPECT_EQ(release.error().kind, UpdateErrorKind::bad_release) << tag;
        EXPECT_EQ(release.error().message,
                  std::format("The latest release could not be read: the tag \"{}\" is not vX.Y.Z.", tag));
    }
}

TEST(UpdaterRelease, Unreadable_answers_and_missing_fields_are_bad_releases)
{
    const auto config = config_for("C:\\Data");
    const auto message_of = [&](std::string_view json) {
        const auto release = update::parse_release(json, config);
        EXPECT_FALSE(release.has_value()) << json;
        if (release) {
            return std::string{};
        }
        EXPECT_EQ(release.error().kind, UpdateErrorKind::bad_release) << json;
        EXPECT_FALSE(release.error().http_status.has_value());
        return release.error().message;
    };
    EXPECT_EQ(message_of("<html>rate limited</html>"), "The latest release could not be read: it is not JSON.");
    EXPECT_EQ(message_of("{\"tag_name\":\"v1.2.0\""), "The latest release could not be read: it is not JSON.");
    EXPECT_EQ(message_of("[]"), "The latest release could not be read: it is not a JSON object.");
    EXPECT_EQ(message_of(R"({"html_url":"https://github.com/x"})"),
              "The latest release could not be read: tag_name is missing.");
    EXPECT_EQ(message_of(R"({"tag_name":1,"html_url":"https://github.com/x"})"),
              "The latest release could not be read: tag_name is missing.");
    EXPECT_EQ(message_of(R"({"tag_name":"v1.2.0"})"), "The latest release could not be read: html_url is missing.");
}

// =====================================================================================
// check_latest
// =====================================================================================

TEST(UpdaterCheck, Sends_one_GitHub_API_request_with_the_exact_headers)
{
    Harness h{[](const RecordedRequest&) { return wizz::test::json_response(ReleaseJson{}.text()); }};
    const auto release = h.updater.check_latest({});
    ASSERT_TRUE(release.has_value()) << describe(release);
    EXPECT_EQ(release->version, (Version{1, 2, 0}));
    EXPECT_TRUE(release->newer);

    const auto requests = h.transport->requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].method, "GET");
    EXPECT_EQ(requests[0].url, kLatestUrl);
    EXPECT_EQ(requests[0].body, std::nullopt);
    EXPECT_EQ(requests[0].headers, (http::Headers{{"User-Agent", std::string{kUserAgent}},
                                                  {"Accept", "application/vnd.github+json"},
                                                  {"X-GitHub-Api-Version", "2022-11-28"}}));
    EXPECT_TRUE(h.delay.delays().empty());
}

TEST(UpdaterCheck, Not_found_means_no_release_is_published)
{
    Harness h{[](const RecordedRequest&) { return wizz::test::json_response(R"({"message":"Not Found"})", 404); }};
    const auto release = h.updater.check_latest({});
    ASSERT_FALSE(release.has_value());
    EXPECT_EQ(release.error().kind, UpdateErrorKind::no_release);
    EXPECT_EQ(release.error().http_status, 404);
    EXPECT_EQ(release.error().message, "No release is published yet (GitHub answered HTTP 404).");
    EXPECT_EQ(h.transport->request_count(), 1u);
}

TEST(UpdaterCheck, Other_statuses_and_network_errors_fail_once_without_retrying)
{
    {
        Harness h{[](const RecordedRequest&) { return wizz::test::json_response(R"({"message":"x"})", 503); }};
        const auto release = h.updater.check_latest({});
        ASSERT_FALSE(release.has_value());
        EXPECT_EQ(release.error().kind, UpdateErrorKind::http_status);
        EXPECT_EQ(release.error().http_status, 503);
        EXPECT_EQ(release.error().message, "Update check failed: GitHub answered HTTP 503.");
        ASSERT_EQ(release.error().tries.size(), 1u);
        EXPECT_EQ(release.error().tries[0].status, 503);
        EXPECT_EQ(h.transport->request_count(), 1u);
    }
    {
        Harness h{[](const RecordedRequest&) -> ScriptedResponse {
            throw http::TransportError("Could not resolve host: api.github.com");
        }};
        const auto release = h.updater.check_latest({});
        ASSERT_FALSE(release.has_value());
        EXPECT_EQ(release.error().kind, UpdateErrorKind::network);
        EXPECT_EQ(release.error().http_status, std::nullopt);
        EXPECT_EQ(release.error().message,
                  "Update check failed: network error: Could not resolve host: api.github.com");
        EXPECT_EQ(h.transport->request_count(), 1u);
    }
    {
        Harness h{[](const RecordedRequest&) -> ScriptedResponse { throw http::TimeoutError("Timeout was reached"); }};
        const auto release = h.updater.check_latest({});
        ASSERT_FALSE(release.has_value());
        EXPECT_EQ(release.error().kind, UpdateErrorKind::network);
        EXPECT_EQ(release.error().message, "Update check failed: timeout: Timeout was reached");
        EXPECT_TRUE(h.delay.delays().empty());
    }
    {
        Harness h{[](const RecordedRequest&) { return wizz::test::json_response(R"({"message":"x"})", 403); }};
        const auto release = h.updater.check_latest({});
        ASSERT_FALSE(release.has_value());
        EXPECT_EQ(release.error().kind, UpdateErrorKind::http_status);
        EXPECT_EQ(release.error().http_status, 403);
    }
}

TEST(UpdaterCheck, A_200_with_a_bad_body_keeps_its_status)
{
    Harness h{[](const RecordedRequest&) { return wizz::test::html_response("<html>busy</html>"); }};
    const auto release = h.updater.check_latest({});
    ASSERT_FALSE(release.has_value());
    EXPECT_EQ(release.error().kind, UpdateErrorKind::bad_release);
    EXPECT_EQ(release.error().http_status, 200);
    ASSERT_EQ(release.error().tries.size(), 1u);
}

TEST(UpdaterCheck, Logs_the_try_and_the_outcome_in_the_updates_folder)
{
    const std::string body = ReleaseJson{}.text();
    Harness h{[&](const RecordedRequest&) { return wizz::test::json_response(body); }};
    (void)h.updater.check_latest({});

    EXPECT_FALSE(fs::exists(h.temp.path() / "data" / "update.log"));
    EXPECT_EQ(h.log_lines(), (std::vector<std::string>{
                                 std::format("{} GET {} -> HTTP 200, {} bytes, 0 ms", kNowIso, kLatestUrl, body.size()),
                                 std::format("{} Update check: latest release v1.2.0, running 1.0.0.", kNowIso),
                             }));
    EXPECT_TRUE(h.log().ends_with("\r\n"));
}

TEST(UpdaterCheck, Cancellation_throws_and_records_nothing)
{
    Harness h{[](const RecordedRequest&) { return wizz::test::json_response(ReleaseJson{}.text()); }};
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW((void)h.updater.check_latest(stop.get_token()), wizz::OperationCancelled);
}

// =====================================================================================
// SHA256SUMS.txt and SHA-256
// =====================================================================================

constexpr std::string_view kHashA = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr std::string_view kHashB = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

TEST(UpdaterChecksums, Parses_two_spaces_and_binary_marker_lines)
{
    const auto lines = update::parse_sha256sums(std::format("{}  a.zip\n{} *b setup.exe\n", kHashA, kHashB));
    ASSERT_TRUE(lines.has_value()) << lines.error();
    EXPECT_EQ(*lines, (std::vector<update::ChecksumLine>{{std::string{kHashA}, "a.zip"},
                                                         {std::string{kHashB}, "b setup.exe"}}));
    EXPECT_EQ(update::find_checksum(*lines, "b setup.exe"), kHashB);
    EXPECT_EQ(update::find_checksum(*lines, "A.ZIP"), std::nullopt);
    EXPECT_EQ(update::find_checksum(*lines, "c.zip"), std::nullopt);
}

TEST(UpdaterChecksums, Accepts_crlf_a_bom_blank_lines_and_upper_case_hex)
{
    std::string upper{kHashB};
    for (char& c : upper) {
        c = static_cast<char>(c >= 'a' && c <= 'f' ? c - 'a' + 'A' : c);
    }
    const auto lines =
        update::parse_sha256sums(std::format("\xEF\xBB\xBF{}  a.zip\r\n\r\n{}  b.exe\r\n\n", kHashA, upper));
    ASSERT_TRUE(lines.has_value()) << lines.error();
    EXPECT_EQ(*lines, (std::vector<update::ChecksumLine>{{std::string{kHashA}, "a.zip"},
                                                         {std::string{kHashB}, "b.exe"}}));
    // The same file twice with the same hash is harmless; no final line break is fine.
    EXPECT_TRUE(update::parse_sha256sums(std::format("{}  a.zip\n{}  a.zip", kHashA, kHashA)).has_value());
    EXPECT_TRUE(update::parse_sha256sums("").has_value());
}

TEST(UpdaterChecksums, Rejects_malformed_lines_and_conflicting_duplicates)
{
    const std::string a{kHashA};
    const auto error_of = [](const std::string& text) {
        const auto lines = update::parse_sha256sums(text);
        EXPECT_FALSE(lines.has_value()) << text;
        return lines ? std::string{} : lines.error();
    };
    EXPECT_EQ(error_of(std::format("{}  ok.zip\n{} one-space.zip\n", a, a)), "line 2");
    EXPECT_EQ(error_of(a.substr(1) + "  short.zip\n"), "line 1");
    EXPECT_EQ(error_of(a + "0  long.zip\n"), "line 1");
    EXPECT_EQ(error_of("g" + a.substr(1) + "  not-hex.zip\n"), "line 1");
    EXPECT_EQ(error_of(a + "  \n"), "line 1");                    // no file name
    EXPECT_EQ(error_of(a + "\tTAB.zip\n"), "line 1");
    EXPECT_EQ(error_of(std::format("{}  x.zip\n{}  x.zip\n", kHashA, kHashB)), "line 2");
}

TEST(UpdaterChecksums, Sha256_matches_known_vectors_and_files)
{
    EXPECT_EQ(update::sha256_hex(""), kHashA);
    EXPECT_EQ(update::sha256_hex("abc"), kHashB);
    const std::string big(3 * 1024 * 1024 + 17, 'x');
    EXPECT_EQ(update::sha256_hex(big), wizz::test::sha256_hex(big));

    TempDirectory temp;
    write_file(temp.path() / "big.bin", big);
    EXPECT_EQ(update::sha256_file(temp.path() / "big.bin"), wizz::test::sha256_hex(big));
    write_file(temp.path() / "empty.bin", "");
    EXPECT_EQ(update::sha256_file(temp.path() / "empty.bin"), kHashA);
    EXPECT_EQ(update::sha256_file(temp.path() / "missing.bin"), std::nullopt);
}

// =====================================================================================
// download_verified
// =====================================================================================

TEST(UpdaterDownload, Fetches_the_checksums_first_then_the_asset_and_verifies_it)
{
    Harness h{sums_then([](const RecordedRequest&) { return bytes_response(kInstallerBytes); })};
    const auto release = standard_release(h.config);

    const auto file = h.updater.download_verified(release, AssetKind::installer, h.downloads(), {});
    ASSERT_TRUE(file.has_value()) << describe(file);
    EXPECT_EQ(file->path, h.downloads() / kInstaller);
    EXPECT_EQ(file->sha256, wizz::test::sha256_hex(kInstallerBytes));
    EXPECT_FALSE(file->reused);
    EXPECT_EQ(read_file(file->path), kInstallerBytes);
    EXPECT_FALSE(fs::exists(h.downloads() / (std::string{kInstaller} + ".tmp")));

    EXPECT_EQ(h.transport->urls(),
              (std::vector<std::string>{fresh(browser_url(kSums), 1), fresh(browser_url(kInstaller), 2)}));
    for (const auto& request : h.transport->requests()) {
        EXPECT_EQ(request.headers, (http::Headers{{"User-Agent", std::string{kUserAgent}}})) << request.url;
    }
    ASSERT_EQ(file->tries.size(), 2u);
    EXPECT_EQ(file->tries[1].url, fresh(browser_url(kInstaller), 2));
    EXPECT_EQ(file->tries[1].status, 200);
    EXPECT_EQ(file->tries[1].bytes, kInstallerBytes.size());
    EXPECT_TRUE(h.delay.delays().empty());
}

TEST(UpdaterDownload, The_portable_zip_downloads_the_same_way)
{
    Harness h{sums_then([](const RecordedRequest&) { return bytes_response(kZipBytes); })};
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::portable_zip,
                                                  h.downloads(), {});
    ASSERT_TRUE(file.has_value()) << describe(file);
    EXPECT_EQ(file->path, h.downloads() / kZip);
    EXPECT_EQ(read_file(file->path), kZipBytes);
}

TEST(UpdaterDownload, A_file_that_does_not_match_is_deleted)
{
    Harness h{sums_then([](const RecordedRequest&) { return bytes_response("tampered bytes"); })};
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().kind, UpdateErrorKind::checksum_mismatch);
    EXPECT_EQ(file.error().http_status, 200);
    EXPECT_EQ(file.error().message,
              std::format("{} does not match SHA256SUMS.txt (expected {}, got {}); the file was deleted.", kInstaller,
                          wizz::test::sha256_hex(kInstallerBytes), wizz::test::sha256_hex("tampered bytes")));
    EXPECT_EQ(file.error().tries.size(), 2u);
    EXPECT_FALSE(fs::exists(h.downloads() / kInstaller));
    EXPECT_FALSE(fs::exists(h.downloads() / (std::string{kInstaller} + ".tmp")));
    EXPECT_EQ(h.log_lines().back(), std::format("{} {}", kNowIso, file.error().message));
}

TEST(UpdaterDownload, Checksums_without_the_assets_line_stop_before_the_asset)
{
    const std::string sums = std::format("{}  {}\n", wizz::test::sha256_hex(kZipBytes), kZip);
    Harness h{sums_then([](const RecordedRequest&) { return bytes_response(kInstallerBytes); }, sums)};
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().kind, UpdateErrorKind::bad_checksums);
    EXPECT_EQ(file.error().message, std::format("SHA256SUMS.txt of v1.2.0 has no line for {}.", kInstaller));
    EXPECT_EQ(file.error().http_status, 200);
    EXPECT_EQ(h.transport->request_count(), 1u);
}

TEST(UpdaterDownload, Malformed_checksums_stop_before_the_asset)
{
    Harness h{sums_then([](const RecordedRequest&) { return bytes_response(kInstallerBytes); },
                        "not a checksum file\n")};
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().kind, UpdateErrorKind::bad_checksums);
    EXPECT_EQ(file.error().message, "SHA256SUMS.txt of v1.2.0 is malformed (line 1).");
    EXPECT_EQ(h.transport->request_count(), 1u);
}

TEST(UpdaterDownload, A_missing_asset_or_checksum_file_sends_nothing)
{
    Harness h{[](const RecordedRequest&) { return bytes_response(kInstallerBytes); }};
    ReleaseInfo no_installer = standard_release(h.config);
    no_installer.installer.reset();
    const auto a = h.updater.download_verified(no_installer, AssetKind::installer, h.downloads(), {});
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().kind, UpdateErrorKind::missing_asset);
    EXPECT_EQ(a.error().message, std::format("Release v1.2.0 has no {}.", kInstaller));

    ReleaseInfo no_sums = standard_release(h.config);
    no_sums.checksums.reset();
    const auto b = h.updater.download_verified(no_sums, AssetKind::installer, h.downloads(), {});
    ASSERT_FALSE(b.has_value());
    EXPECT_EQ(b.error().kind, UpdateErrorKind::missing_asset);
    EXPECT_EQ(b.error().message, "Release v1.2.0 has no SHA256SUMS.txt.");
    EXPECT_EQ(h.transport->request_count(), 0u);
}

TEST(UpdaterDownload, Retries_5xx_408_429_and_network_errors_with_a_fresh_query_each_time)
{
    int asset_tries = 0;
    Harness h{sums_then([&](const RecordedRequest&) -> ScriptedResponse {
        switch (++asset_tries) {
        case 1: return status_response(502);
        case 2: return status_response(408);
        case 3: return status_response(429);
        case 4: throw http::TransportError("Recv failure: Connection was reset");
        default: return bytes_response(kInstallerBytes);
        }
    })};
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
    ASSERT_TRUE(file.has_value()) << describe(file);

    // Three tries on the browser URL, then the API URL; the counter is shared with the checksum file.
    EXPECT_EQ(h.transport->urls(),
              (std::vector<std::string>{fresh(browser_url(kSums), 1), fresh(browser_url(kInstaller), 2),
                                        fresh(browser_url(kInstaller), 3), fresh(browser_url(kInstaller), 4),
                                        fresh(api_url(kInstallerId), 5), fresh(api_url(kInstallerId), 6)}));
    const auto urls = h.transport->urls();
    EXPECT_EQ(std::set<std::string>(urls.begin(), urls.end()).size(), urls.size()) << "every try needs its own URL";
    EXPECT_EQ(h.delay.delays(), (std::vector<Duration>{2s, 4s, 8s, 16s}));
    EXPECT_EQ(read_file(h.downloads() / kInstaller), kInstallerBytes);
}

TEST(UpdaterDownload, Timeouts_and_server_errors_on_the_checksum_file_are_retried_too)
{
    int sums_tries = 0;
    Harness h{[&](const RecordedRequest& request) -> ScriptedResponse {
        if (request.url.contains(kSums)) {
            if (++sums_tries == 1) {
                throw http::TimeoutError("Operation timed out after 30000 milliseconds");
            }
            if (sums_tries == 2) {
                return status_response(500);
            }
            return bytes_response(sums_text());
        }
        return bytes_response(kInstallerBytes);
    }};
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
    ASSERT_TRUE(file.has_value()) << describe(file);
    EXPECT_EQ(h.transport->urls(),
              (std::vector<std::string>{fresh(browser_url(kSums), 1), fresh(browser_url(kSums), 2),
                                        fresh(browser_url(kSums), 3), fresh(browser_url(kInstaller), 4)}));
    EXPECT_EQ(h.delay.delays(), (std::vector<Duration>{2s, 4s}));
}

TEST(UpdaterDownload, Not_found_and_forbidden_are_not_retried_but_fall_back_to_the_api_url)
{
    for (const int status : {404, 403}) {
        int asset_tries = 0;
        Harness h{sums_then([&](const RecordedRequest&) {
            return ++asset_tries == 1 ? status_response(status) : bytes_response(kInstallerBytes);
        })};
        const auto file =
            h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
        ASSERT_TRUE(file.has_value()) << status << ": " << describe(file);
        EXPECT_EQ(h.transport->urls(),
                  (std::vector<std::string>{fresh(browser_url(kSums), 1), fresh(browser_url(kInstaller), 2),
                                            fresh(api_url(kInstallerId), 3)}))
            << status;
        EXPECT_TRUE(h.delay.delays().empty()) << status;
    }
}

TEST(UpdaterDownload, The_api_url_asks_for_octet_stream)
{
    Harness h{sums_then([](const RecordedRequest& request) {
        return request.url.starts_with(kApiAssetBase) ? bytes_response(kInstallerBytes) : status_response(503);
    })};
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
    ASSERT_TRUE(file.has_value()) << describe(file);

    const auto requests = h.transport->requests();
    ASSERT_EQ(requests.size(), 5u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(requests[i].headers, (http::Headers{{"User-Agent", std::string{kUserAgent}}})) << i;
    }
    EXPECT_EQ(requests[4].url, fresh(api_url(kInstallerId), 5));
    EXPECT_EQ(requests[4].headers, (http::Headers{{"User-Agent", std::string{kUserAgent}},
                                                  {"Accept", "application/octet-stream"},
                                                  {"X-GitHub-Api-Version", "2022-11-28"}}));
    EXPECT_EQ(h.delay.delays(), (std::vector<Duration>{2s, 4s, 8s}));
}

TEST(UpdaterDownload, Gives_up_after_every_try_with_the_last_status)
{
    Harness h{sums_then([](const RecordedRequest&) { return status_response(503); })};
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().kind, UpdateErrorKind::http_status);
    EXPECT_EQ(file.error().http_status, 503);
    EXPECT_EQ(file.error().message, std::format("Could not download {} after 5 tries: HTTP 503.", kInstaller));
    EXPECT_EQ(file.error().tries.size(), 6u);  // the checksum file and five asset tries
    EXPECT_EQ(h.transport->request_count(), 6u);
    EXPECT_EQ(h.delay.delays(), (std::vector<Duration>{2s, 4s, 8s, 16s}));
    EXPECT_FALSE(fs::exists(h.downloads() / kInstaller));
}

TEST(UpdaterDownload, A_final_network_error_still_names_the_last_status_seen)
{
    int asset_tries = 0;
    Harness h{sums_then([&](const RecordedRequest& request) -> ScriptedResponse {
        ++asset_tries;
        if (request.url.starts_with(kApiAssetBase)) {
            throw http::TransportError("SSL connect error");
        }
        return asset_tries == 1 ? status_response(404) : status_response(500);
    })};
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().kind, UpdateErrorKind::network);
    EXPECT_EQ(file.error().http_status, 404);
    EXPECT_EQ(file.error().message,
              std::format("Could not download {} after 3 tries: network error: SSL connect error", kInstaller));
}

TEST(UpdaterDownload, Forbidden_everywhere_ends_after_one_try_per_url)
{
    Harness h{sums_then([](const RecordedRequest&) { return status_response(403); })};
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().kind, UpdateErrorKind::http_status);
    EXPECT_EQ(file.error().http_status, 403);
    EXPECT_EQ(file.error().message, std::format("Could not download {} after 2 tries: HTTP 403.", kInstaller));
    EXPECT_EQ(h.transport->request_count(), 3u);
    EXPECT_TRUE(h.delay.delays().empty());
}

TEST(UpdaterDownload, Every_try_is_logged_with_method_url_status_bytes_and_duration)
{
    std::shared_ptr<FixedClock> clock;  // set below; the responder makes each try take 1.5 s
    int asset_tries = 0;
    Harness h{sums_then([&](const RecordedRequest&) -> ScriptedResponse {
        clock->advance(1500ms);
        if (++asset_tries == 1) {
            throw http::TransportError("Couldn't connect to server");
        }
        return bytes_response(kInstallerBytes);
    })};
    clock = h.clock;
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
    ASSERT_TRUE(file.has_value()) << describe(file);

    const std::string sums = sums_text();
    ASSERT_EQ(file->tries.size(), 3u);
    EXPECT_EQ(file->tries[0], (update::TryRecord{.started_at = FixedClock::kDefaultUtcNow,
                                                 .method = "GET",
                                                 .url = fresh(browser_url(kSums), 1),
                                                 .status = 200,
                                                 .error = "",
                                                 .bytes = sums.size(),
                                                 .duration = 0ms}));
    EXPECT_EQ(file->tries[1], (update::TryRecord{.started_at = FixedClock::kDefaultUtcNow,
                                                 .method = "GET",
                                                 .url = fresh(browser_url(kInstaller), 2),
                                                 .status = std::nullopt,
                                                 .error = "network error: Couldn't connect to server",
                                                 .bytes = 0,
                                                 .duration = 1500ms}));
    EXPECT_EQ(file->tries[2].status, 200);
    EXPECT_EQ(file->tries[2].duration, 1500ms);
    EXPECT_EQ(file->tries[2].started_at, FixedClock::kDefaultUtcNow + 1500ms);

    const std::string ms_after_first = "1793613601500";
    EXPECT_EQ(h.log_lines(),
              (std::vector<std::string>{
                  std::format("{} GET {} -> HTTP 200, {} bytes, 0 ms", kNowIso, fresh(browser_url(kSums), 1),
                              sums.size()),
                  std::format("{} GET {} -> network error: Couldn't connect to server, 0 bytes, 1500 ms", kNowIso,
                              fresh(browser_url(kInstaller), 2)),
                  std::format("2026-11-02T10:00:01.5000000Z GET {}?attempt=3&t={} -> HTTP 200, {} bytes, 1500 ms",
                              browser_url(kInstaller), ms_after_first, kInstallerBytes.size()),
                  std::format("2026-11-02T10:00:03Z Verified {} (SHA-256 {}).", kInstaller,
                              wizz::test::sha256_hex(kInstallerBytes)),
              }));
}

TEST(UpdaterDownload, Cancelling_during_a_back_off_stops_at_once)
{
    std::stop_source stop;
    Harness h{sums_then([](const RecordedRequest&) { return status_response(503); }),
              [&](Duration) { stop.request_stop(); }};
    EXPECT_THROW((void)h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(),
                                                   stop.get_token()),
                 wizz::OperationCancelled);
    EXPECT_EQ(h.transport->request_count(), 2u);
    EXPECT_EQ(h.delay.delays(), (std::vector<Duration>{2s}));
    EXPECT_FALSE(fs::exists(h.downloads() / kInstaller));
    EXPECT_EQ(h.log_lines().back(), std::format("{} Cancelled.", kNowIso));
}

TEST(UpdaterDownload, Cancelling_during_a_transfer_leaves_no_file)
{
    std::stop_source stop;
    Harness h{sums_then([&](const RecordedRequest&) -> ScriptedResponse {
        stop.request_stop();
        throw wizz::OperationCancelled{};
    })};
    EXPECT_THROW((void)h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(),
                                                   stop.get_token()),
                 wizz::OperationCancelled);
    EXPECT_EQ(h.transport->request_count(), 2u);
    EXPECT_FALSE(fs::exists(h.downloads() / kInstaller));
    EXPECT_FALSE(fs::exists(h.downloads() / (std::string{kInstaller} + ".tmp")));
    const auto lines = h.log_lines();
    ASSERT_GE(lines.size(), 2u);
    EXPECT_EQ(lines[lines.size() - 2],
              std::format("{} GET {} -> cancelled, 0 bytes, 0 ms", kNowIso, fresh(browser_url(kInstaller), 2)));
}

TEST(UpdaterDownload, A_timeout_while_cancelling_is_a_cancellation)
{
    std::stop_source stop;
    Harness h{sums_then([&](const RecordedRequest&) -> ScriptedResponse {
        stop.request_stop();
        throw http::TimeoutError("Timeout was reached");
    })};
    EXPECT_THROW((void)h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(),
                                                   stop.get_token()),
                 wizz::OperationCancelled);
    EXPECT_EQ(h.transport->request_count(), 2u);
    EXPECT_TRUE(h.delay.delays().empty());
}

TEST(UpdaterDownload, A_verified_earlier_download_is_kept_and_a_bad_one_replaced)
{
    {
        Harness h{sums_then([](const RecordedRequest&) { return bytes_response(kInstallerBytes); })};
        write_file(h.downloads() / kInstaller, kInstallerBytes);
        const auto file =
            h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
        ASSERT_TRUE(file.has_value()) << describe(file);
        EXPECT_TRUE(file->reused);
        EXPECT_EQ(file->tries.size(), 1u);
        EXPECT_EQ(h.transport->request_count(), 1u);  // only SHA256SUMS.txt
        EXPECT_EQ(h.log_lines().back(),
                  std::format("{} Kept {}, already downloaded and verified (SHA-256 {}).", kNowIso, kInstaller,
                              wizz::test::sha256_hex(kInstallerBytes)));
    }
    {
        Harness h{sums_then([](const RecordedRequest&) { return bytes_response(kInstallerBytes); })};
        write_file(h.downloads() / kInstaller, "half a download");
        const auto file =
            h.updater.download_verified(standard_release(h.config), AssetKind::installer, h.downloads(), {});
        ASSERT_TRUE(file.has_value()) << describe(file);
        EXPECT_FALSE(file->reused);
        EXPECT_EQ(read_file(h.downloads() / kInstaller), kInstallerBytes);
        EXPECT_EQ(h.transport->request_count(), 2u);
    }
}

TEST(UpdaterDownload, An_unusable_destination_is_a_file_system_error)
{
    Harness h{sums_then([](const RecordedRequest&) { return bytes_response(kInstallerBytes); })};
    const fs::path blocked = h.temp.path() / "a-file";
    write_file(blocked, "not a directory");
    const auto file = h.updater.download_verified(standard_release(h.config), AssetKind::installer, blocked, {});
    ASSERT_FALSE(file.has_value());
    EXPECT_EQ(file.error().kind, UpdateErrorKind::file_system);
    EXPECT_TRUE(file.error().message.starts_with(std::format("Could not save {} in ", kInstaller)))
        << file.error().message;
    EXPECT_EQ(file.error().http_status, 200);
    EXPECT_EQ(read_file(blocked), "not a directory");
}

TEST(UpdaterDownload, Fresh_query_appends_to_an_existing_query)
{
    const auto now = FixedClock::kDefaultUtcNow;
    EXPECT_EQ(update::with_fresh_query("https://x/a", 1, now), std::format("https://x/a?attempt=1&t={}", kNowMs));
    EXPECT_EQ(update::with_fresh_query("https://x/a?b=c", 7, now + 1ms),
              "https://x/a?b=c&attempt=7&t=1793613600001");
    EXPECT_TRUE(update::is_retryable_status(500));
    EXPECT_TRUE(update::is_retryable_status(599));
    EXPECT_TRUE(update::is_retryable_status(408));
    EXPECT_TRUE(update::is_retryable_status(429));
    for (const int status : {200, 301, 400, 401, 403, 404, 410, 422, 499, 600}) {
        EXPECT_FALSE(update::is_retryable_status(status)) << status;
    }
}

// =====================================================================================
// The update log
// =====================================================================================

TEST(UpdaterLog, Rotates_a_large_log_and_keeps_lines_on_one_line)
{
    TempDirectory temp;
    const fs::path log = temp.path() / "updates" / "update.log";
    update::append_log_line(log, FixedClock::kDefaultUtcNow, "first\r\nsecond");
    EXPECT_EQ(read_file(log), std::format("{} first  second\r\n", kNowIso));

    const std::string big(update::kLogRotateBytes + 1, 'x');
    write_file(log, big);
    update::append_log_line(log, FixedClock::kDefaultUtcNow, "fresh");
    EXPECT_EQ(read_file(log), std::format("{} fresh\r\n", kNowIso));
    EXPECT_EQ(read_file(temp.path() / "updates" / "update.log.old"), big);
}

// =====================================================================================
// Daily schedule
// =====================================================================================

const wizz::UtcInstant kT0 = FixedClock::kDefaultUtcNow;

TEST(UpdaterSchedule, Due_without_a_successful_check_in_the_last_24_hours)
{
    EXPECT_TRUE(update::is_check_due(UpdateState{}, kT0));

    const UpdateState checked{.last_success = kT0, .latest_seen = Version{1, 2, 0}, .last_failure = std::nullopt};
    EXPECT_FALSE(update::is_check_due(checked, kT0));
    EXPECT_FALSE(update::is_check_due(checked, kT0 + 23h + 59min + 59s));
    EXPECT_TRUE(update::is_check_due(checked, kT0 + 24h));
    EXPECT_TRUE(update::is_check_due(checked, kT0 + 24h * 30));
}

TEST(UpdaterSchedule, A_failed_check_waits_an_hour_before_the_next_try)
{
    const UpdateState failed{.last_success = std::nullopt, .latest_seen = std::nullopt, .last_failure = kT0};
    EXPECT_FALSE(update::is_check_due(failed, kT0));
    EXPECT_FALSE(update::is_check_due(failed, kT0 + 59min + 59s));
    EXPECT_TRUE(update::is_check_due(failed, kT0 + 1h));

    const UpdateState old_success_then_failure{
        .last_success = kT0 - 48h, .latest_seen = Version{1, 0, 0}, .last_failure = kT0};
    EXPECT_FALSE(update::is_check_due(old_success_then_failure, kT0 + 30min));
    EXPECT_TRUE(update::is_check_due(old_success_then_failure, kT0 + 1h));
}

TEST(UpdaterSchedule, Times_in_the_future_do_not_block_a_check)
{
    const UpdateState future{.last_success = kT0 + 72h, .latest_seen = std::nullopt, .last_failure = kT0 + 2h};
    EXPECT_TRUE(update::is_check_due(future, kT0));
}

TEST(UpdaterSchedule, State_round_trips_through_its_json)
{
    const UpdateState state{.last_success = kT0, .latest_seen = Version{1, 2, 0}, .last_failure = kT0 + 90min};
    EXPECT_EQ(state.to_json(), "{\r\n"
                               "  \"LastSuccessfulCheckUtc\": \"2026-11-02T10:00:00Z\",\r\n"
                               "  \"LatestSeenVersion\": \"1.2.0\",\r\n"
                               "  \"LastFailedCheckUtc\": \"2026-11-02T11:30:00Z\"\r\n"
                               "}");
    EXPECT_EQ(UpdateState::parse(state.to_json()), state);
    EXPECT_EQ(UpdateState{}.to_json(), "{}");
    EXPECT_EQ(UpdateState::parse("{}"), UpdateState{});

    const UpdateState fraction{.last_success = kT0 + 250ms, .latest_seen = std::nullopt, .last_failure = std::nullopt};
    EXPECT_EQ(UpdateState::parse(fraction.to_json()), fraction);
}

TEST(UpdaterSchedule, Malformed_state_is_empty)
{
    for (const std::string_view bad : {
             "", "not json", "[]", "null",
             R"({"LastSuccessfulCheckUtc":5})",
             R"({"LastSuccessfulCheckUtc":"yesterday"})",
             R"({"LastSuccessfulCheckUtc":"2026-11-02T10:00:00"})",       // not UTC
             R"({"LastSuccessfulCheckUtc":"2026-11-02T10:00:00+01:00"})",
             R"({"LatestSeenVersion":"v1.2.0"})",
             R"({"LastFailedCheckUtc":true})",
         }) {
        EXPECT_EQ(UpdateState::parse(bad), UpdateState{}) << bad;
    }
    // Unknown members are ignored.
    EXPECT_EQ(UpdateState::parse(R"({"Other":1,"LatestSeenVersion":"1.0.1"})").latest_seen, (Version{1, 0, 1}));
}

TEST(UpdaterSchedule, Records_successes_and_failures_in_the_updates_folder)
{
    TempDirectory temp;
    const auto clock = std::make_shared<FixedClock>();
    const UpdateConfig config = config_for(temp.path());
    const update::UpdateSchedule schedule{config.updates_dir(), clock};

    EXPECT_EQ(schedule.load(), UpdateState{});
    EXPECT_TRUE(schedule.is_check_due());

    schedule.record_failure();
    EXPECT_EQ(schedule.load(), (UpdateState{.last_success = std::nullopt, .latest_seen = std::nullopt,
                                            .last_failure = kT0}));
    EXPECT_FALSE(schedule.is_check_due());
    clock->advance(1h);
    EXPECT_TRUE(schedule.is_check_due());

    schedule.record_success(Version{1, 2, 0});
    EXPECT_EQ(schedule.load(), (UpdateState{.last_success = kT0 + 1h, .latest_seen = Version{1, 2, 0},
                                            .last_failure = std::nullopt}));
    EXPECT_TRUE(fs::is_regular_file(temp.path() / "updates" / "update-state.json"));
    EXPECT_FALSE(fs::exists(temp.path() / "update-state.json"));
    EXPECT_FALSE(fs::exists(temp.path() / "settings.json"));
    EXPECT_FALSE(schedule.is_check_due());

    clock->advance(24h);
    schedule.record_failure();
    EXPECT_EQ(schedule.load(), (UpdateState{.last_success = kT0 + 1h, .latest_seen = Version{1, 2, 0},
                                            .last_failure = kT0 + 25h}));

    write_file(config.state_file(), "garbage");
    EXPECT_EQ(schedule.load(), UpdateState{});
}

TEST(UpdaterSchedule, Records_check_results)
{
    TempDirectory temp;
    const auto clock = std::make_shared<FixedClock>();
    const update::UpdateSchedule schedule{temp.path(), clock};
    const auto config = config_for(temp.path());

    schedule.record(update::parse_release(ReleaseJson{}.text(), config));
    EXPECT_EQ(schedule.load().latest_seen, (Version{1, 2, 0}));
    EXPECT_EQ(schedule.load().last_success, kT0);

    ReleaseJson draft;
    draft.draft = true;
    clock->advance(24h);
    schedule.record(update::parse_release(draft.text(), config));  // no_release: a successful check
    EXPECT_EQ(schedule.load(), (UpdateState{.last_success = kT0 + 24h, .latest_seen = std::nullopt,
                                            .last_failure = std::nullopt}));

    clock->advance(24h);
    schedule.record(update::parse_release("junk", config));
    EXPECT_EQ(schedule.load().last_failure, kT0 + 48h);
    EXPECT_EQ(schedule.load().last_success, kT0 + 24h);
}

TEST(UpdaterSchedule, Check_if_due_sends_nothing_when_not_due_and_records_the_result)
{
    int status = 200;
    Harness h{[&](const RecordedRequest&) { return wizz::test::json_response(ReleaseJson{}.text(), status); }};
    const update::UpdateSchedule schedule{h.config.updates_dir(), h.clock};

    const auto first = h.updater.check_if_due(schedule, {});
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->has_value());
    EXPECT_EQ(schedule.load().latest_seen, (Version{1, 2, 0}));

    h.clock->advance(23h);
    EXPECT_FALSE(h.updater.check_if_due(schedule, {}).has_value());
    EXPECT_EQ(h.transport->request_count(), 1u);

    h.clock->advance(1h);
    status = 502;
    const auto failed = h.updater.check_if_due(schedule, {});
    ASSERT_TRUE(failed.has_value());
    EXPECT_FALSE(failed->has_value());
    EXPECT_EQ(schedule.load().last_failure, kT0 + 24h);

    h.clock->advance(30min);
    EXPECT_FALSE(h.updater.check_if_due(schedule, {}).has_value());
    EXPECT_EQ(h.transport->request_count(), 2u);

    std::stop_source stop;
    stop.request_stop();
    h.clock->advance(30min);
    EXPECT_THROW((void)h.updater.check_if_due(schedule, stop.get_token()), wizz::OperationCancelled);
    EXPECT_EQ(schedule.load().last_failure, kT0 + 24h);  // a cancelled check records nothing
}

// =====================================================================================
// Installing
// =====================================================================================

TEST(UpdaterInstall, Installed_when_the_uninstaller_sits_next_to_the_exe)
{
    TempDirectory temp;
    const fs::path exe = temp.path() / "WizzairHelper.exe";
    write_file(exe, "MZ");
    EXPECT_EQ(update::detect_install_kind(exe), InstallKind::portable);

    fs::create_directories(temp.path() / "unins000.exe");  // a directory does not count
    EXPECT_EQ(update::detect_install_kind(exe), InstallKind::portable);
    fs::remove(temp.path() / "unins000.exe");

    write_file(temp.path() / "sub" / "unins000.exe", "MZ");  // nor one elsewhere
    EXPECT_EQ(update::detect_install_kind(exe), InstallKind::portable);

    write_file(temp.path() / "unins000.exe", "MZ");
    EXPECT_EQ(update::detect_install_kind(exe), InstallKind::installed);
    EXPECT_EQ(update::asset_kind_for(InstallKind::installed), AssetKind::installer);
    EXPECT_EQ(update::asset_kind_for(InstallKind::portable), AssetKind::portable_zip);
}

TEST(UpdaterInstall, The_running_executable_is_this_test_binary)
{
    const fs::path exe = update::running_executable_path();
    EXPECT_EQ(exe.filename(), L"wizzairhelper_tests.exe");
    EXPECT_TRUE(fs::is_regular_file(exe));
    EXPECT_EQ(update::detect_install_kind(exe), InstallKind::portable);
}

TEST(UpdaterInstall, Silent_install_command_follows_the_contract)
{
    const fs::path setup = L"C:\\Users\\Ann Lee\\AppData\\Local\\WizzairHelper\\updates\\downloads\\"
                           L"WizzairHelper-1.2.0-win64-setup.exe";
    const auto relaunch = update::silent_install_command(setup, update::Relaunch::yes);
    EXPECT_EQ(relaunch.program, setup);
    EXPECT_EQ(relaunch.arguments, (std::vector<std::string>{"/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                                                            "/CLOSEAPPLICATIONS", "/RELAUNCH"}));
    EXPECT_EQ(relaunch.parameters(), "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS /RELAUNCH");
    EXPECT_EQ(relaunch.command_line(),
              L"\"C:\\Users\\Ann Lee\\AppData\\Local\\WizzairHelper\\updates\\downloads\\"
              L"WizzairHelper-1.2.0-win64-setup.exe\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS "
              L"/RELAUNCH");

    const auto quiet = update::silent_install_command(L"D:\\setup.exe", update::Relaunch::no);
    EXPECT_EQ(quiet.parameters(), "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS");
    EXPECT_EQ(quiet.command_line(), L"\"D:\\setup.exe\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS");
}

TEST(UpdaterInstall, Removes_only_stale_downloads)
{
    TempDirectory temp;
    const UpdateConfig config = config_for(temp.path(), "1.2.0");
    const fs::path dir = config.download_dir();
    const std::vector<std::string> stale{"WizzairHelper-1.1.0-win64-setup.exe", "WizzairHelper-1.2.0-win64.zip",
                                         "WizzairHelper-0.9.9-win64-setup.exe.tmp", "WizzairHelper-1.2.0-win64.zip.tmp"};
    const std::vector<std::string> kept{"WizzairHelper-1.3.0-win64-setup.exe", "WizzairHelper-1.10.0-win64.zip",
                                        "notes.txt", "WizzairHelper-1.0-win64.zip", "SHA256SUMS.txt",
                                        "WizzairHelper-1.1.0-win64-setup.exe.bak"};
    for (const auto& name : stale) {
        write_file(dir / name, "x");
    }
    for (const auto& name : kept) {
        write_file(dir / name, "x");
    }
    fs::create_directories(dir / "WizzairHelper-1.0.0-win64.zip");  // a directory is never touched

    EXPECT_EQ(update::remove_stale_downloads(config), stale.size());
    for (const auto& name : stale) {
        EXPECT_FALSE(fs::exists(dir / name)) << name;
    }
    for (const auto& name : kept) {
        EXPECT_TRUE(fs::exists(dir / name)) << name;
    }
    EXPECT_TRUE(fs::is_directory(dir / "WizzairHelper-1.0.0-win64.zip"));

    // No download folder yet: nothing to do.
    EXPECT_EQ(update::remove_stale_downloads(config_for(temp.path() / "elsewhere")), 0u);
}

} // namespace
