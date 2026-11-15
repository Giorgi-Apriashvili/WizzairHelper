// settings.json (01 §13, 06 §3) and how the panel applies it (01 §4.1, §4.4).
// Files live in a per-test temporary directory, never in the user profile.

#include "wizz/ui/settings.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

using namespace wizz::ui;
namespace fs = std::filesystem;

/// A fresh temporary directory, removed afterwards (I/O errors ignored).
class TempDir {
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("wizzairhelper-ui-test-" + std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(++counter));
        std::error_code error;
        fs::remove_all(path_, error);
    }
    ~TempDir()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void write_file(const fs::path& path, std::string_view bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

constexpr std::string_view kExample = R"({
  "From": "Budapest",
  "To": "London",
  "FromRadiusKm": 250,
  "ToRadiusKm": 100,
  "Adults": 1,
  "DiscountClub": false,
  "Currency": "EUR",
  "MinNights": 2,
  "MaxNights": 10,
  "WindowWidth": 1320,
  "WindowHeight": 820,
  "Maximized": false
})";

TEST(Settings, Defaults)
{
    const AppSettings settings;
    EXPECT_EQ(settings.from, "Budapest");
    EXPECT_EQ(settings.to, "London");
    EXPECT_EQ(settings.from_radius_km, 250);
    EXPECT_EQ(settings.to_radius_km, 100);
    EXPECT_EQ(settings.adults, 1);
    EXPECT_FALSE(settings.discount_club);
    EXPECT_EQ(settings.currency, "EUR");
    EXPECT_EQ(settings.min_nights, 2);
    EXPECT_EQ(settings.max_nights, 10);
    EXPECT_EQ(settings.window_width, 0);
    EXPECT_EQ(settings.window_height, 0);
    EXPECT_FALSE(settings.maximized);
}

TEST(Settings, Reads_the_example_of_01_13)
{
    AppSettings expected;
    expected.window_width = 1320;
    expected.window_height = 820;
    EXPECT_EQ(AppSettings::parse(kExample), expected);
}

TEST(Settings, Writes_the_example_in_table_order_indented_two_spaces)
{
    AppSettings settings;
    settings.window_width = 1320;
    settings.window_height = 820;
    std::string expected;
    for (const char c : kExample) {
        if (c == '\n') {
            expected += "\r\n";  // Environment.NewLine (json::Layout::indented)
        } else {
            expected += c;
        }
    }
    EXPECT_EQ(settings.to_json(), expected);
}

TEST(Settings, Every_value_round_trips_including_unicode)
{
    AppSettings settings;
    settings.from = "Krak\u00F3w \U0001F600";
    settings.to = "";
    settings.from_radius_km = 1500;
    settings.to_radius_km = 0;
    settings.adults = 9;
    settings.discount_club = true;
    settings.currency = "HUF";
    settings.min_nights = 0;
    settings.max_nights = 120;
    settings.window_width = 1920;
    settings.window_height = 1040;
    settings.maximized = true;
    const std::string text = settings.to_json();
    // JsonSerializer escaping: every non-ASCII character as an upper-case UCN, a surrogate pair beyond the BMP
    // (\x5C below is a backslash).
    EXPECT_NE(text.find("\"From\": \"Krak\x5Cu00F3w \x5CuD83D\x5CuDE00\""), std::string::npos);
    EXPECT_EQ(AppSettings::parse(text), settings);
}

TEST(Settings, Missing_properties_keep_defaults_and_unknown_ones_are_ignored)
{
    AppSettings expected;
    expected.adults = 3;
    EXPECT_EQ(AppSettings::parse(R"({"Adults": 3, "Foo": [1, {"a": null}], "Bar": "x"})"), expected);
    EXPECT_EQ(AppSettings::parse("{}"), AppSettings{});
    EXPECT_EQ(AppSettings::parse(" null "), AppSettings{});  // null root → defaults
}

TEST(Settings, Property_names_are_case_sensitive)
{
    EXPECT_EQ(AppSettings::parse(R"({"from": "Wien", "ADULTS": 4})"), AppSettings{});
}

TEST(Settings, Duplicate_names_last_wins)
{
    AppSettings expected;
    expected.adults = 4;
    EXPECT_EQ(AppSettings::parse(R"({"Adults": 2, "Adults": 4})"), expected);
    // but every occurrence is read: a bad first value still fails the file
    EXPECT_EQ(AppSettings::parse(R"({"Adults": "x", "Adults": 4})"), AppSettings{});
}

TEST(Settings, Json_null_for_a_string_gives_empty_text)
{
    AppSettings expected;
    expected.from = "";
    expected.currency = "";
    expected.max_nights = 12;
    EXPECT_EQ(AppSettings::parse(R"({"From": null, "Currency": null, "MaxNights": 12})"), expected);
}

TEST(Settings, Any_type_or_syntax_error_gives_all_defaults)
{
    const std::string_view bad[] = {
        R"({"MaxNights": 12, "Adults": "two"})",
        R"({"MaxNights": 12, "FromRadiusKm": 250.5})",
        R"({"MaxNights": 12, "FromRadiusKm": 250.0})",
        R"({"MaxNights": 12, "FromRadiusKm": 1e2})",
        R"({"MaxNights": 12, "FromRadiusKm": "250"})",
        R"({"MaxNights": 12, "FromRadiusKm": 2147483648})",
        R"({"MaxNights": 12, "Adults": null})",
        R"({"MaxNights": 12, "DiscountClub": null})",
        R"({"MaxNights": 12, "DiscountClub": "true"})",
        R"({"MaxNights": 12, "DiscountClub": 1})",
        R"({"MaxNights": 12, "From": 5})",
        R"({"MaxNights": 12, "To": ["London"]})",
        R"({"MaxNights": 12, /* comment */ "Adults": 2})",
        R"({"MaxNights": 12, "Adults": 2,})",
        R"([{"MaxNights": 12}])",
        R"(12)",
        R"("settings")",
        R"({"MaxNights": 12)",
        "",
        "   ",
    };
    for (const std::string_view text : bad) {
        EXPECT_EQ(AppSettings::parse(text), AppSettings{}) << text;
    }
}

TEST(Settings, Unicode_escapes_and_surrogate_pairs_are_decoded)
{
    // JSON escapes in the file text (\x5C is a backslash): lower- and upper-case hex, a surrogate pair.
    const AppSettings settings =
        AppSettings::parse("{\"From\": \"Gda\x5Cu0144sk \x5Cud83d\x5Cude00\", \"To\": \"\x5Cu00c1rhus\"}");
    EXPECT_EQ(settings.from, "Gda\u0144sk \U0001F600");
    EXPECT_EQ(settings.to, "\u00C1rhus");
}

TEST(Settings, Unpaired_surrogates_fail_the_file_only_where_they_are_read)
{
    // D-28 (verified on .NET 10): the serializer turns GetString's InvalidOperationException
    // into a JsonException, so a known string or any property name holding an unpaired
    // surrogate escape gives all defaults; an unknown property's value is skipped unread.
    EXPECT_EQ(AppSettings::parse("{\"MaxNights\": 12, \"From\": \"\\ud800\"}"), AppSettings{});
    EXPECT_EQ(AppSettings::parse("{\"MaxNights\": 12, \"\\udc00\": 1}"), AppSettings{});
    AppSettings expected;
    expected.max_nights = 12;
    EXPECT_EQ(AppSettings::parse("{\"MaxNights\": 12, \"Comment\": \"\\ud800\"}"), expected);
}

TEST(Settings, Values_are_not_clamped_on_load)
{
    const AppSettings settings = AppSettings::parse(R"({"FromRadiusKm": -5, "Adults": 12, "MinNights": 70})");
    EXPECT_EQ(settings.from_radius_km, -5);
    EXPECT_EQ(settings.adults, 12);
    EXPECT_EQ(settings.min_nights, 70);
}

TEST(Settings, Load_handles_missing_unreadable_and_bom_prefixed_files)
{
    const TempDir dir;
    EXPECT_EQ(AppSettings::load(dir.path() / "settings.json"), AppSettings{});  // missing
    fs::create_directories(dir.path() / "folder.json");
    EXPECT_EQ(AppSettings::load(dir.path() / "folder.json"), AppSettings{});  // not a file

    write_file(dir.path() / "bom.json", "\xEF\xBB\xBF{\"Adults\": 5}");
    EXPECT_EQ(AppSettings::load(dir.path() / "bom.json").adults, 5);
    write_file(dir.path() / "bad.json", "{\"Adults\": 5,}");
    EXPECT_EQ(AppSettings::load(dir.path() / "bad.json"), AppSettings{});
}

TEST(Settings, Save_creates_the_directory_and_replaces_atomically)
{
    const TempDir dir;
    const fs::path file = dir.path() / "WizzairHelper" / "settings.json";
    AppSettings settings;
    settings.from = "Wien";
    settings.window_width = 1400;
    ASSERT_TRUE(settings.save(file));
    EXPECT_EQ(read_file(file), settings.to_json());
    EXPECT_EQ(AppSettings::load(file), settings);

    settings.adults = 4;
    ASSERT_TRUE(settings.save(file));  // replaces the previous file
    EXPECT_EQ(AppSettings::load(file), settings);
    std::size_t entries = 0;
    for ([[maybe_unused]] const fs::directory_entry& entry : fs::directory_iterator(file.parent_path())) {
        ++entries;
    }
    EXPECT_EQ(entries, 1u);  // no temporary file left behind
}

TEST(Settings, Save_errors_are_reported_not_thrown)
{
    const TempDir dir;
    fs::create_directories(dir.path() / "settings.json" / "sub");  // the target is a non-empty directory
    EXPECT_FALSE(AppSettings{}.save(dir.path() / "settings.json"));
}

TEST(Settings, Initial_inputs_clamp_like_the_panel)
{
    AppSettings settings;
    EXPECT_EQ(initial_inputs(settings), (InitialInputs{.from = "Budapest",
                                                       .to = "London",
                                                       .from_radius_km = 250,
                                                       .to_radius_km = 100,
                                                       .adults = 1,
                                                       .fare_index = 0,
                                                       .currency = "EUR",
                                                       .min_nights = 2,
                                                       .max_nights = 10}));
    settings.from_radius_km = 2000;
    settings.to_radius_km = -5;
    settings.adults = 0;
    settings.discount_club = true;
    settings.currency = "eur";  // case-sensitive list match → EUR
    settings.min_nights = 70;
    settings.max_nights = 5;
    const InitialInputs clamped = initial_inputs(settings);
    EXPECT_EQ(clamped.from_radius_km, 1500);
    EXPECT_EQ(clamped.to_radius_km, 0);
    EXPECT_EQ(clamped.adults, 1);
    EXPECT_EQ(clamped.fare_index, 1);
    EXPECT_EQ(clamped.currency, "EUR");
    EXPECT_EQ(clamped.min_nights, 60);
    EXPECT_EQ(clamped.max_nights, 60);  // max(clamp(5, 0, 120), min)

    settings.adults = 12;
    settings.currency = "ILS";
    settings.max_nights = 130;
    settings.min_nights = 3;
    EXPECT_EQ(initial_inputs(settings).adults, 9);
    EXPECT_EQ(initial_inputs(settings).currency, "ILS");
    EXPECT_EQ(initial_inputs(settings).max_nights, 120);
    settings.currency = "XYZ";
    EXPECT_EQ(initial_inputs(settings).currency, "EUR");
    settings.currency = "";
    EXPECT_EQ(initial_inputs(settings).currency, "EUR");
}

TEST(Settings, Numeric_editors_round_for_display_and_truncate_for_use)
{
    EXPECT_EQ(numeric_display_value(2.5), 3.0);
    EXPECT_EQ(numeric_display_value(2.7), 3.0);
    EXPECT_EQ(numeric_display_value(2.4), 2.0);
    EXPECT_EQ(truncate_toward_zero(2.7), 2);
    EXPECT_EQ(truncate_toward_zero(-2.7), -2);
    EXPECT_EQ(truncate_toward_zero(1e12), 2147483647);
}

TEST(Settings, UX_15_Start_size_is_clamped_to_the_work_area)
{
    const WindowSize fallback{1320, 820};
    const WindowSize minimum{980, 600};
    const WindowSize work{1920, 1040};
    AppSettings settings;
    EXPECT_EQ(start_window_size(settings, fallback, minimum, work), fallback);
    settings.window_width = 3000;
    settings.window_height = 2000;
    EXPECT_EQ(start_window_size(settings, fallback, minimum, work), (WindowSize{1920, 1040}));
    settings.window_width = 500;
    settings.window_height = 300;
    EXPECT_EQ(start_window_size(settings, fallback, minimum, work), (WindowSize{980, 600}));
    settings.window_width = 1200;
    settings.window_height = 0;
    EXPECT_EQ(start_window_size(settings, fallback, minimum, work), (WindowSize{1200, 600}));
    EXPECT_EQ(start_window_size(settings, fallback, minimum, WindowSize{800, 500}), (WindowSize{980, 600}));

    settings.maximized = true;
    EXPECT_TRUE(start_maximized(settings, false));
    EXPECT_FALSE(start_maximized(settings, true));  // never in autotest
}

} // namespace
