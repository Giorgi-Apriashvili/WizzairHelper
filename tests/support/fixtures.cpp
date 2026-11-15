#include "support/fixtures.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "bcrypt.lib")  // BCryptHash for sha256_hex (keeps tests/CMakeLists.txt unchanged)

namespace wizz::test {

namespace {

[[nodiscard]] std::filesystem::path executable_path()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("GetModuleFileNameW failed.");
        }
        if (length < buffer.size()) {
            return std::filesystem::path{std::wstring{buffer.data(), length}};
        }
        buffer.resize(buffer.size() * 2);  // truncated: retry with a larger buffer
    }
}

/// The whole file `directory/name`, byte-exact.
[[nodiscard]] std::string read_file_in(const std::filesystem::path& directory, std::string_view name)
{
    const std::filesystem::path path = directory / std::filesystem::path{std::u8string{name.begin(), name.end()}};
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error(std::format("Cannot open fixture {}", path.string()));
    }
    std::string content{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (file.bad()) {
        throw std::runtime_error(std::format("Cannot read fixture {}", path.string()));
    }
    return content;
}

} // namespace

std::filesystem::path fixtures_dir()
{
    return executable_path().parent_path() / "Fixtures";
}

std::string read_fixture(std::string_view name)
{
    return read_file_in(fixtures_dir(), name);
}

std::filesystem::path live_cache_dir()
{
    return executable_path().parent_path() / "LiveCache";
}

std::string read_live_cache(std::string_view name)
{
    return read_file_in(live_cache_dir(), name);
}

std::string sha256_hex(std::string_view bytes)
{
    constexpr ULONG kDigestSize = 32;
    UCHAR digest[kDigestSize]{};
    // BCryptHash takes a non-const input pointer but only reads through it.
    const NTSTATUS status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                       reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                                       static_cast<ULONG>(bytes.size()), digest, kDigestSize);
    if (!BCRYPT_SUCCESS(status)) {
        throw std::runtime_error(std::format("BCryptHash failed with status {:#x}", static_cast<unsigned long>(status)));
    }
    std::string hex;
    hex.reserve(2 * kDigestSize);
    for (const UCHAR byte : digest) {
        hex += std::format("{:02x}", byte);
    }
    return hex;
}

std::string home_html_for(std::string_view version)
{
    constexpr std::string_view kVersion = "29.23.1";
    std::string html{kHomeHtml};
    html.replace(html.find(kVersion), kVersion.size(), version);
    return html;
}

Responder default_responder(std::string_view timetable_fixture)
{
    return [map = read_fixture("map_subset.json"),
            timetable = read_fixture(timetable_fixture)](const RecordedRequest& request) {
        if (request.url.starts_with("https://www.wizzair.com")) {
            return html_response(std::string{kHomeHtml});
        }
        if (request.url.contains("/asset/map")) {
            return json_response(map);
        }
        if (request.url.contains("/search/timetableV2")) {
            return json_response(timetable);
        }
        return json_response(R"({"message":"No HTTP resource was found"})", 404);
    };
}

} // namespace wizz::test
