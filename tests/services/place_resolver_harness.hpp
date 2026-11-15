#pragma once
// Shared harness of the place-resolver tests (05 §3.9): the `Resolver(photon)`
// fake transport, notice collection, exact-type exception checks, expected
// geocoder URLs and an isolated temporary cache directory for geocode.json.

#include "wizz/core/cancellation.hpp"
#include "wizz/core/clocks.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/progress.hpp"
#include "wizz/services/file_cache.hpp"
#include "wizz/services/place_resolver.hpp"

#include "support/fake_transport.hpp"
#include "support/recording_delay.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <numbers>
#include <random>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

namespace wizz::test::geocoder {

/// Collects notices synchronously on the reporting thread (05 §1.8 SyncProgress).
class NoticeLog {
public:
    [[nodiscard]] NoticeSink sink()
    {
        return [this](const ClientNotice& notice) {
            const std::scoped_lock lock(mutex_);
            notices_.push_back(notice);
        };
    }
    [[nodiscard]] std::vector<ClientNotice> notices() const
    {
        const std::scoped_lock lock(mutex_);
        return notices_;
    }
    /// The messages, in report order.
    [[nodiscard]] std::vector<std::string> messages() const
    {
        std::vector<std::string> result;
        for (const ClientNotice& notice : notices()) {
            result.push_back(notice.message);
        }
        return result;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ClientNotice> notices_;
};

/// `Looking up "{text}" (Photon geocoder)` (02 §6.4, 01 C9; ASCII quotes).
[[nodiscard]] inline std::string photon_notice(std::string_view text)
{
    return std::format("Looking up \"{}\" (Photon geocoder)", text);
}
/// `Looking up "{text}" (OpenStreetMap Nominatim)` (02 §6.4, 01 C10).
[[nodiscard]] inline std::string nominatim_notice(std::string_view text)
{
    return std::format("Looking up \"{}\" (OpenStreetMap Nominatim)", text);
}
/// 05 §3.9 Photon URL with an already escaped query.
[[nodiscard]] inline std::string photon(std::string_view escaped_query)
{
    return std::format("https://photon.komoot.io/api/?q={}&limit=6&lang=en&layer=city", escaped_query);
}
/// 05 §3.9 Nominatim URL with an already escaped query.
[[nodiscard]] inline std::string nominatim(std::string_view escaped_query)
{
    return std::format(
        "https://nominatim.openstreetmap.org/search?q={}&format=jsonv2&limit=6&accept-language=en&featureType=settlement",
        escaped_query);
}

/// The "offline" network failure (HttpRequestException("offline")).
inline ScriptedResponse offline(const RecordedRequest& /*request*/)
{
    throw http::TransportError("offline");
}

/// 05 §3.9 `Resolver(photon)` routing: URLs containing `photon` go to `photon_answer`,
/// every other URL (Nominatim) gets 200 JSON `[]`.
[[nodiscard]] inline Responder photon_responder(std::function<ScriptedResponse(const RecordedRequest&)> photon_answer)
{
    return [answer = std::move(photon_answer)](const RecordedRequest& request) {
        if (request.url.find("photon") != std::string::npos) {
            return answer(request);
        }
        return json_response("[]");
    };
}

/// Photon answering a fixed JSON body.
[[nodiscard]] inline Responder photon_answers(std::string body)
{
    return photon_responder([body = std::move(body)](const RecordedRequest&) { return json_response(body); });
}

/// A resolver over a fake transport with the FIXED clock (05 §1.1) and a recording
/// Nominatim delay; the cache is disabled unless a test supplies one.
struct ResolverRig {
    explicit ResolverRig(Responder responder, FileCache cache = FileCache::disabled(),
                         std::shared_ptr<FixedClock> fixed_clock = std::make_shared<FixedClock>())
        : transport(std::make_shared<FakeTransport>(std::move(responder))),
          clock(std::move(fixed_clock)),
          resolver(transport, std::move(cache), clock, delay.delay_func())
    {
    }

    std::shared_ptr<FakeTransport> transport;
    std::shared_ptr<FixedClock> clock;
    RecordingDelay delay;
    PlaceResolver resolver;
    NoticeLog log;

    [[nodiscard]] PlaceLookup resolve(std::string_view text, const AirportCatalog& catalog,
                                      std::stop_token stop = {})
    {
        return resolver.resolve(text, catalog, log.sink(), std::move(stop));
    }
};

/// The thrown error's kind must be exactly `Expected` (05 §1.9 Assert.ThrowsAsync<T>).
template <typename Expected, typename Fn>
void expect_throws_exactly(Fn&& fn, std::string_view expected_message = {})
{
    try {
        std::forward<Fn>(fn)();
    } catch (const std::exception& error) {
        EXPECT_EQ(typeid(error), typeid(Expected)) << "threw " << typeid(error).name() << ": " << error.what();
        if (!expected_message.empty()) {
            EXPECT_EQ(std::string_view{error.what()}, expected_message);
        }
        return;
    }
    ADD_FAILURE() << "nothing was thrown, expected " << typeid(Expected).name();
}

/// The point `km` kilometres due north of `from` along its meridian. The haversine of a
/// pure latitude difference is R·|Δφ|, so distance_km(from, north_of(from, km)) ≈ km.
[[nodiscard]] inline GeoPoint north_of(GeoPoint from, double km)
{
    return GeoPoint{from.latitude + km / (kEarthRadiusKm * std::numbers::pi / 180.0), from.longitude};
}

/// A fresh directory under %TEMP% (`wizzairhelper-test-<random>`), removed afterwards (05 §1.5).
class TempDirectory {
public:
    TempDirectory()
    {
        std::random_device entropy;
        path_ = std::filesystem::temp_directory_path() /
                std::format("wizzairhelper-test-{:08x}{:08x}", entropy(), entropy());
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] FileCache cache() const { return FileCache{path_}; }
    [[nodiscard]] std::filesystem::path geocode_file() const { return path_ / "geocode.json"; }

private:
    std::filesystem::path path_;
};

inline void write_bytes(const std::filesystem::path& path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] inline std::string read_bytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// Sets the file's last-write time to an exact UTC instant (FILETIME = 100 ns ticks since 1601).
inline void set_last_write_utc(const std::filesystem::path& path, UtcInstant instant)
{
    const auto since_1601 = instant - UtcInstant{std::chrono::sys_days{std::chrono::year{1601} / 1 / 1}};
    const auto ticks = static_cast<std::uint64_t>(since_1601.count());
    const FILETIME time{.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFu),
                        .dwHighDateTime = static_cast<DWORD>(ticks >> 32)};
    const HANDLE file = ::CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(file, INVALID_HANDLE_VALUE);
    const BOOL set = ::SetFileTime(file, nullptr, nullptr, &time);
    ::CloseHandle(file);
    ASSERT_TRUE(set);
}

} // namespace wizz::test::geocoder
