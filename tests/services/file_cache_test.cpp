// FileCache and AppPaths (04 §6, 02 §7, 06 §3). Every test works in its own
// temporary directory; nothing touches the user profile.

#include "wizz/services/app_paths.hpp"
#include "wizz/services/file_cache.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using namespace std::string_literals;

/// 05 §1.1 FixedTime: 2026-11-02T10:00:00Z, never advances, local zone UTC.
class FixedClock final : public wizz::Clock {
public:
    explicit FixedClock(wizz::UtcInstant now) : now_(now) {}

    [[nodiscard]] wizz::UtcInstant utc_now() const override { return now_; }
    [[nodiscard]] wizz::LocalDateTime local_now() const override
    {
        return wizz::LocalDateTime{now_.time_since_epoch()};
    }
    [[nodiscard]] wizz::Ticks monotonic_now() const override { return wizz::Ticks::zero(); }

private:
    wizz::UtcInstant now_;
};

const wizz::UtcInstant kNow = std::chrono::sys_days{std::chrono::year{2026} / 11 / 2} + 10h;

/// A fresh directory under %TEMP%, removed with everything in it afterwards.
class TempDirectory {
public:
    TempDirectory()
    {
        std::random_device entropy;
        path_ = fs::temp_directory_path() / std::format("wizzairhelper-test-{:08x}{:08x}", entropy(), entropy());
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

/// Keeps a file open without any sharing, like another process holding it locked.
class ExclusiveLock {
public:
    explicit ExclusiveLock(const fs::path& path)
        : handle_(::CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
    }
    ~ExclusiveLock()
    {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;

    [[nodiscard]] bool locked() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

std::string read_bytes(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const fs::path& path, std::string_view bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// Sets the file's last-write time to an exact UTC instant (100 ns FILETIME ticks).
void set_last_write_utc(const fs::path& path, wizz::UtcInstant instant)
{
    const auto since_1601 = instant - wizz::UtcInstant{std::chrono::sys_days{std::chrono::year{1601} / 1 / 1}};
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

// --- disabled cache (05 §1.5) ---------------------------------------------------------

TEST(FileCache, Disabled_cache_reads_nothing_and_writes_nothing)
{
    const FixedClock clock{kNow};
    const wizz::FileCache cache = wizz::FileCache::disabled();

    EXPECT_FALSE(cache.directory().has_value());
    cache.try_write("map.json", "{}");
    cache.try_delete("map.json");
    EXPECT_EQ(cache.try_read("map.json", std::nullopt, clock), std::nullopt);
    EXPECT_EQ(cache.try_read("map.json", 24h, clock), std::nullopt);
    EXPECT_EQ(cache.written_at("map.json"), std::nullopt);
}

// --- reading and writing (04 §6) --------------------------------------------------------

TEST(FileCache, Write_then_read_round_trips_utf8_without_bom)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    const std::string content = R"({"graz":[{"Name":"Graz","Detail":"Kraków – Zürich"}]})";

    cache.try_write("geocode.json", content);

    EXPECT_EQ(cache.try_read("geocode.json", std::nullopt, clock), content);
    EXPECT_EQ(read_bytes(temp.path() / "geocode.json"), content); // exact bytes, no BOM
    EXPECT_FALSE(fs::exists(temp.path() / "geocode.json.tmp"));
}

TEST(FileCache, Api_url_is_written_exactly_without_newline_CT_API_14)
{
    const TempDirectory temp;
    const wizz::FileCache cache{temp.path()};

    cache.try_write("api-url.txt", "https://be.wizzair.com/29.23.1/Api");

    EXPECT_EQ(read_bytes(temp.path() / "api-url.txt"), "https://be.wizzair.com/29.23.1/Api");
}

TEST(FileCache, Write_replaces_an_existing_file_and_a_stale_tmp)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    write_bytes(temp.path() / "map.json", "old map, longer than the new one");
    write_bytes(temp.path() / "map.json.tmp", "leftover from an interrupted write");

    cache.try_write("map.json", "new");

    EXPECT_EQ(cache.try_read("map.json", std::nullopt, clock), "new");
    EXPECT_EQ(read_bytes(temp.path() / "map.json"), "new");
    EXPECT_FALSE(fs::exists(temp.path() / "map.json.tmp"));
}

TEST(FileCache, Write_creates_missing_directories)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const fs::path nested = temp.path() / "WizzairHelper" / "cache";
    const wizz::FileCache cache{nested};

    cache.try_write("fx.json", "[]");

    EXPECT_TRUE(fs::is_regular_file(nested / "fx.json"));
    EXPECT_EQ(cache.try_read("fx.json", std::nullopt, clock), "[]");
}

TEST(FileCache, Empty_content_round_trips)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};

    cache.try_write("empty.txt", "");

    EXPECT_EQ(cache.try_read("empty.txt", std::nullopt, clock), "");
}

TEST(FileCache, Missing_file_or_directory_name_reads_as_nullopt)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    fs::create_directory(temp.path() / "folder.json");

    EXPECT_EQ(cache.try_read("missing.json", std::nullopt, clock), std::nullopt);
    EXPECT_EQ(cache.try_read("folder.json", std::nullopt, clock), std::nullopt);
    EXPECT_EQ(wizz::FileCache{temp.path() / "no-such-dir"}.try_read("map.json", std::nullopt, clock), std::nullopt);
}

TEST(FileCache, Unicode_directory_and_file_names)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const fs::path directory = temp.path() / L"Benutzer-üñïçødé-日本";
    const wizz::FileCache cache{directory};

    cache.try_write("géocode-€.json", "été");

    EXPECT_TRUE(fs::is_regular_file(directory / L"géocode-€.json"));
    EXPECT_EQ(cache.try_read("géocode-€.json", std::nullopt, clock), "été");
    cache.try_delete("géocode-€.json");
    EXPECT_FALSE(fs::exists(directory / L"géocode-€.json"));
}

// --- text decoding like File.ReadAllText ---------------------------------------------------

TEST(FileCache, Read_detects_and_strips_byte_order_marks)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    write_bytes(temp.path() / "utf8.json", "\xEF\xBB\xBF{\"a\":\"\xC3\xA9\"}");
    write_bytes(temp.path() / "utf16.json", "\xFF\xFE{\0}\0\xE9\0"s);
    write_bytes(temp.path() / "utf16be.json", "\xFE\xFF\0{\0}"s);

    EXPECT_EQ(cache.try_read("utf8.json", std::nullopt, clock), "{\"a\":\"\xC3\xA9\"}");
    EXPECT_EQ(cache.try_read("utf16.json", std::nullopt, clock), "{}\xC3\xA9");
    EXPECT_EQ(cache.try_read("utf16be.json", std::nullopt, clock), "{}");
}

TEST(FileCache, Corrupt_bytes_become_replacement_characters)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    write_bytes(temp.path() / "map.json", "{\"x\":\"\xC3\x28\xFF\"}");

    EXPECT_EQ(cache.try_read("map.json", std::nullopt, clock), "{\"x\":\"\xEF\xBF\xBD(\xEF\xBF\xBD\"}");
}

// --- freshness (04 §6: stale only when now - lastWrite > maxAge) -----------------------------

TEST(FileCache, Age_equal_to_max_age_is_fresh_one_tick_older_is_stale)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    const fs::path file = temp.path() / "map.json";
    cache.try_write("map.json", "{\"cities\":[]}");

    set_last_write_utc(file, kNow - 24h);
    EXPECT_EQ(cache.try_read("map.json", 24h, clock), "{\"cities\":[]}");

    set_last_write_utc(file, kNow - 24h - wizz::Ticks{1});
    EXPECT_EQ(cache.try_read("map.json", 24h, clock), std::nullopt);
    EXPECT_EQ(cache.try_read("map.json", std::nullopt, clock), "{\"cities\":[]}") << "no max age: any age is fine";
}

TEST(FileCache, Future_write_time_is_fresh)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    cache.try_write("fx.json", "[]");

    set_last_write_utc(temp.path() / "fx.json", kNow + 365 * 24h);

    EXPECT_EQ(cache.try_read("fx.json", 12h, clock), "[]");
    EXPECT_EQ(cache.try_read("fx.json", wizz::Duration::zero(), clock), "[]");
}

TEST(FileCache, Written_at_is_the_exact_last_write_time_D_71)
{
    const TempDirectory temp;
    const wizz::FileCache cache{temp.path()};
    cache.try_write("api-url.txt", "https://be.wizzair.com/29.26.0/Api");

    set_last_write_utc(temp.path() / "api-url.txt", kNow - 24h - wizz::Ticks{1});
    EXPECT_EQ(cache.written_at("api-url.txt"), kNow - 24h - wizz::Ticks{1});
    set_last_write_utc(temp.path() / "api-url.txt", kNow + 365 * 24h);  // a future time is reported as is
    EXPECT_EQ(cache.written_at("api-url.txt"), kNow + 365 * 24h);

    EXPECT_EQ(cache.written_at("missing.txt"), std::nullopt);
    fs::create_directory(temp.path() / "folder");
    EXPECT_EQ(cache.written_at("folder"), std::nullopt) << "a directory is not a cache file";
}

TEST(FileCache, Ninety_day_geocode_ttl_boundary)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    cache.try_write("geocode.json", "{}");
    constexpr auto kNinetyDays = std::chrono::days{90};

    set_last_write_utc(temp.path() / "geocode.json", kNow - kNinetyDays);
    EXPECT_EQ(cache.try_read("geocode.json", kNinetyDays, clock), "{}");
    set_last_write_utc(temp.path() / "geocode.json", kNow - kNinetyDays - 1s);
    EXPECT_EQ(cache.try_read("geocode.json", kNinetyDays, clock), std::nullopt);
}

// --- errors are swallowed (04 §6, 06 §3) ------------------------------------------------------

TEST(FileCache, Locked_file_reads_as_nullopt)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    cache.try_write("map.json", "{}");

    const ExclusiveLock lock{temp.path() / "map.json"};
    ASSERT_TRUE(lock.locked());
    EXPECT_EQ(cache.try_read("map.json", std::nullopt, clock), std::nullopt);
}

TEST(FileCache, Failed_replace_keeps_the_old_file_and_leaves_the_tmp)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    cache.try_write("map.json", "old");
    {
        const ExclusiveLock lock{temp.path() / "map.json"};
        ASSERT_TRUE(lock.locked());
        cache.try_write("map.json", "new"); // must not throw
        cache.try_delete("map.json");       // must not throw
    }
    EXPECT_EQ(cache.try_read("map.json", std::nullopt, clock), "old");
    EXPECT_EQ(read_bytes(temp.path() / "map.json.tmp"), "new"); // not cleaned up, like .NET
}

TEST(FileCache, Concurrent_writer_holding_the_tmp_makes_the_write_a_no_op)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    cache.try_write("map.json", "first");
    write_bytes(temp.path() / "map.json.tmp", "other instance");

    const ExclusiveLock lock{temp.path() / "map.json.tmp"};
    ASSERT_TRUE(lock.locked());
    cache.try_write("map.json", "second");

    EXPECT_EQ(cache.try_read("map.json", std::nullopt, clock), "first");
}

TEST(FileCache, Unwritable_cache_directory_is_ignored)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    write_bytes(temp.path() / "cache", "a file where the directory should be");
    const wizz::FileCache cache{temp.path() / "cache"};

    cache.try_write("map.json", "{}");
    cache.try_delete("map.json");

    EXPECT_EQ(cache.try_read("map.json", std::nullopt, clock), std::nullopt);
}

TEST(FileCache, Delete_removes_the_file_and_ignores_a_missing_one)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{temp.path()};
    cache.try_write("api-url.txt", "https://be.wizzair.com/29.22.0/Api");

    cache.try_delete("api-url.txt");
    cache.try_delete("api-url.txt");

    EXPECT_FALSE(fs::exists(temp.path() / "api-url.txt"));
    EXPECT_EQ(cache.try_read("api-url.txt", std::nullopt, clock), std::nullopt);
}

// --- AppPaths (04 §6, 01 §3.2, 01 §13) ---------------------------------------------------------

TEST(AppPaths, Default_root_is_local_app_data_wizzairhelper)
{
    const fs::path root = wizz::app_paths::default_root();

    EXPECT_TRUE(root.is_absolute());
    EXPECT_EQ(root.filename(), L"WizzairHelper");
    wchar_t* local_app_data = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&local_app_data, &length, L"LOCALAPPDATA") == 0 && local_app_data != nullptr) {
        const fs::path expected{local_app_data};
        std::free(local_app_data);
        EXPECT_TRUE(fs::equivalent(root.parent_path(), expected)) << root << " vs " << expected;
    }
}

TEST(AppPaths, Files_live_under_the_root)
{
    const fs::path root = wizz::app_paths::default_root();

    EXPECT_EQ(wizz::app_paths::default_cache_dir(), root / "cache");
    EXPECT_EQ(wizz::app_paths::settings_file(), root / "settings.json");
    EXPECT_EQ(wizz::app_paths::errors_log(), root / "errors.log");
}

TEST(AppPaths, Root_override_replaces_the_known_folder)
{
    const TempDirectory temp;
    const std::optional<fs::path> root = temp.path();

    EXPECT_EQ(wizz::app_paths::default_root(root), temp.path());
    EXPECT_EQ(wizz::app_paths::default_cache_dir(root), temp.path() / "cache");
    EXPECT_EQ(wizz::app_paths::settings_file(root), temp.path() / "settings.json");
    EXPECT_EQ(wizz::app_paths::errors_log(root), temp.path() / "errors.log");
}

TEST(AppPaths, Cache_dir_works_with_file_cache)
{
    const TempDirectory temp;
    const FixedClock clock{kNow};
    const wizz::FileCache cache{wizz::app_paths::default_cache_dir(temp.path())};

    cache.try_write("map.json", "{}");

    EXPECT_TRUE(fs::is_regular_file(temp.path() / "cache" / "map.json"));
    EXPECT_EQ(cache.try_read("map.json", std::nullopt, clock), "{}");
}

} // namespace
