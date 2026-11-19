// settings.json (01 §13, 06 §3) and how the panel applies it (01 §4.1, §4.4).
// Files live in a per-test temporary directory, never in the user profile.

#include "wizz/ui/settings.hpp"

#include "wizz/core/json.hpp"

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace wizz::ui;
namespace fs = std::filesystem;
namespace json = wizz::json;
using wizz::fare_days::ReuseWindow;
using wizz::update::CheckFrequency;
using wizz::update::UpdateOptions;
using wizz::update::WhenFound;

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

/// The 01 §13 example, as 2.0 and 2.1 write it: the twelve properties.
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

/// The same settings as this version writes them: the update options follow the twelve (D-60),
/// then the reuse window of fetched fares (D-68) and the distance of Homes on Airbnb (D-73).
constexpr std::string_view kExampleWithUpdates = R"({
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
  "Maximized": false,
  "CheckForUpdates": true,
  "UpdateCheckFrequency": "Daily",
  "WhenUpdateFound": "InstallOnClose",
  "ReuseFaresWithin": "ThirtyMinutes",
  "HomesWithin": "FiveKm"
})";

/// The same settings as 2.2 wrote them: the twelve and the update options.
constexpr std::string_view kExampleOf22 = R"({
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
  "Maximized": false,
  "CheckForUpdates": true,
  "UpdateCheckFrequency": "Daily",
  "WhenUpdateFound": "InstallOnClose"
})";

[[nodiscard]] std::string with_crlf(std::string_view text)
{
    std::string result;
    for (const char c : text) {
        if (c == '\n') {
            result += "\r\n";  // Environment.NewLine (json::Layout::indented)
        } else {
            result += c;
        }
    }
    return result;
}

/// settings.json as 2.1 reads it (its settings.cpp, frozen here): the twelve names,
/// strict types, every other name skipped unread; nullopt where 2.1 falls back to ALL
/// defaults.
struct Settings21 {
    std::string from{"Budapest"};
    std::string to{"London"};
    int from_radius_km = 250;
    int to_radius_km = 100;
    int adults = 1;
    bool discount_club = false;
    std::string currency{"EUR"};
    int min_nights = 2;
    int max_nights = 10;
    int window_width = 0;
    int window_height = 0;
    bool maximized = false;
};

[[nodiscard]] std::optional<Settings21> read_like_2_1(std::string_view text)
{
    try {
        const json::Value root = json::parse(text);
        if (!root.is_object()) {
            return std::nullopt;
        }
        Settings21 settings;
        for (const json::Member& member : root.as_object()) {
            const std::string_view name = member.name();
            const json::Value& value = member.value();
            if (name == "From" || name == "To" || name == "Currency") {
                if (!value.is_null() && !value.is_string()) {
                    return std::nullopt;
                }
                std::string& target = name == "From" ? settings.from : name == "To" ? settings.to : settings.currency;
                target = value.is_null() ? std::string{} : std::string{value.as_string()};
            } else if (name == "FromRadiusKm" || name == "ToRadiusKm" || name == "Adults" || name == "MinNights" ||
                       name == "MaxNights" || name == "WindowWidth" || name == "WindowHeight") {
                const std::optional<std::int32_t> number = value.try_int32();
                if (!number) {
                    return std::nullopt;
                }
                int& target = name == "FromRadiusKm"  ? settings.from_radius_km
                              : name == "ToRadiusKm"  ? settings.to_radius_km
                              : name == "Adults"      ? settings.adults
                              : name == "MinNights"   ? settings.min_nights
                              : name == "MaxNights"   ? settings.max_nights
                              : name == "WindowWidth" ? settings.window_width
                                                      : settings.window_height;
                target = *number;
            } else if (name == "DiscountClub" || name == "Maximized") {
                if (!value.is_bool()) {
                    return std::nullopt;
                }
                (name == "DiscountClub" ? settings.discount_club : settings.maximized) = value.as_bool();
            }
            // 2.1 ignores every other name.
        }
        return settings;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

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
    // The update options default to the behaviour of 2.0 / 2.1 (D-60).
    EXPECT_TRUE(settings.updates.check_automatically);
    EXPECT_EQ(settings.updates.check_frequency, CheckFrequency::daily);
    EXPECT_EQ(settings.updates.when_found, WhenFound::install_on_close);
    // Fares are reused for 30 minutes, as before 2.3 (D-68).
    EXPECT_EQ(settings.fare_reuse, ReuseWindow::thirty_minutes);
}

TEST(Settings, Reads_the_example_of_01_13)
{
    AppSettings expected;
    expected.window_width = 1320;
    expected.window_height = 820;
    EXPECT_EQ(AppSettings::parse(kExample), expected);
}

TEST(Settings, Writes_the_example_in_table_order_indented_two_spaces_then_the_update_options)
{
    AppSettings settings;
    settings.window_width = 1320;
    settings.window_height = 820;
    EXPECT_EQ(settings.to_json(), with_crlf(kExampleWithUpdates));
    EXPECT_EQ(AppSettings::parse(kExampleWithUpdates), settings);
}

TEST(Settings, Update_options_are_written_after_the_twelve_with_their_json_names)
{
    struct Case {
        UpdateOptions options;
        std::string_view tail;
    };
    const Case cases[] = {
        {{.check_automatically = true, .check_frequency = CheckFrequency::daily, .when_found = WhenFound::install_on_close},
         "  \"CheckForUpdates\": true,\r\n  \"UpdateCheckFrequency\": \"Daily\",\r\n  \"WhenUpdateFound\": \"InstallOnClose\",\r\n  \"ReuseFaresWithin\": \"ThirtyMinutes\",\r\n  \"HomesWithin\": \"FiveKm\"\r\n}"},
        {{.check_automatically = false, .check_frequency = CheckFrequency::weekly, .when_found = WhenFound::download_then_ask},
         "  \"CheckForUpdates\": false,\r\n  \"UpdateCheckFrequency\": \"Weekly\",\r\n  \"WhenUpdateFound\": \"DownloadThenAsk\",\r\n  \"ReuseFaresWithin\": \"ThirtyMinutes\",\r\n  \"HomesWithin\": \"FiveKm\"\r\n}"},
        {{.check_automatically = true, .check_frequency = CheckFrequency::monthly, .when_found = WhenFound::notify_only},
         "  \"CheckForUpdates\": true,\r\n  \"UpdateCheckFrequency\": \"Monthly\",\r\n  \"WhenUpdateFound\": \"NotifyOnly\",\r\n  \"ReuseFaresWithin\": \"ThirtyMinutes\",\r\n  \"HomesWithin\": \"FiveKm\"\r\n}"},
    };
    for (const Case& c : cases) {
        AppSettings settings;
        settings.updates = c.options;
        const std::string text = settings.to_json();
        EXPECT_TRUE(text.ends_with("  \"Maximized\": false,\r\n" + std::string{c.tail})) << text;
        EXPECT_EQ(AppSettings::parse(text), settings);
    }
    // Every combination round-trips through the file.
    for (const bool automatic : {true, false}) {
        for (const CheckFrequency frequency : {CheckFrequency::daily, CheckFrequency::weekly, CheckFrequency::monthly}) {
            for (const WhenFound when : {WhenFound::install_on_close, WhenFound::download_then_ask, WhenFound::notify_only}) {
                AppSettings settings;
                settings.updates = UpdateOptions{.check_automatically = automatic, .check_frequency = frequency, .when_found = when};
                EXPECT_EQ(AppSettings::parse(settings.to_json()).updates, settings.updates);
            }
        }
    }
}

TEST(Settings, A_file_of_2_1_reads_the_update_options_as_their_defaults)
{
    AppSettings expected;
    expected.window_width = 1320;
    expected.window_height = 820;
    const AppSettings settings = AppSettings::parse(kExample);
    EXPECT_EQ(settings, expected);
    EXPECT_EQ(settings.updates, UpdateOptions{}) << "check once a day, install when the app closes";
    // Some options present, others missing: the missing ones keep their defaults.
    const AppSettings partial = AppSettings::parse(R"({"Adults": 2, "UpdateCheckFrequency": "Weekly"})");
    EXPECT_EQ(partial.adults, 2);
    EXPECT_EQ(partial.updates, (UpdateOptions{.check_automatically = true,
                                              .check_frequency = CheckFrequency::weekly,
                                              .when_found = WhenFound::install_on_close}));
}

TEST(Settings, A_new_file_read_by_the_rules_of_2_1_keeps_its_twelve_values)
{
    AppSettings settings;
    settings.from = "Wien";
    settings.to = "";
    settings.adults = 3;
    settings.discount_club = true;
    settings.currency = "HUF";
    settings.max_nights = 14;
    settings.window_width = 1500;
    settings.window_height = 900;
    settings.maximized = true;
    settings.updates = UpdateOptions{.check_automatically = false,
                                     .check_frequency = CheckFrequency::monthly,
                                     .when_found = WhenFound::notify_only};
    const std::optional<Settings21> old = read_like_2_1(settings.to_json());
    ASSERT_TRUE(old.has_value()) << "2.1 would fall back to all defaults";
    EXPECT_EQ(old->from, "Wien");
    EXPECT_EQ(old->to, "");
    EXPECT_EQ(old->from_radius_km, 250);
    EXPECT_EQ(old->to_radius_km, 100);
    EXPECT_EQ(old->adults, 3);
    EXPECT_TRUE(old->discount_club);
    EXPECT_EQ(old->currency, "HUF");
    EXPECT_EQ(old->min_nights, 2);
    EXPECT_EQ(old->max_nights, 14);
    EXPECT_EQ(old->window_width, 1500);
    EXPECT_EQ(old->window_height, 900);
    EXPECT_TRUE(old->maximized);
    // The frozen reader is as strict as 2.1: a wrong type among the twelve still resets it.
    EXPECT_FALSE(read_like_2_1(R"({"Adults": "two", "CheckForUpdates": false})").has_value());
}

TEST(Settings, An_unknown_update_option_value_gives_that_options_default_only)
{
    // D-60: a later version may write a value this one does not know.
    AppSettings expected;
    expected.from = "Wien";
    expected.adults = 3;
    expected.updates.check_automatically = false;
    expected.updates.when_found = WhenFound::notify_only;
    EXPECT_EQ(AppSettings::parse(R"({"From": "Wien", "Adults": 3, "CheckForUpdates": false,
                                     "UpdateCheckFrequency": "Hourly", "WhenUpdateFound": "NotifyOnly"})"),
              expected);

    expected.updates = UpdateOptions{.check_automatically = false,
                                     .check_frequency = CheckFrequency::weekly,
                                     .when_found = WhenFound::install_on_close};
    EXPECT_EQ(AppSettings::parse(R"({"From": "Wien", "Adults": 3, "CheckForUpdates": false,
                                     "UpdateCheckFrequency": "Weekly", "WhenUpdateFound": "InstallSilently"})"),
              expected);

    for (const std::string_view unknown : {"", " Weekly", "Week", "Weekly ", "Wéekly", "2"}) {
        const std::string text = R"({"MaxNights": 12, "UpdateCheckFrequency": ")" + std::string{unknown} +
                                 R"(", "WhenUpdateFound": ")" + std::string{unknown} + R"("})";
        const AppSettings settings = AppSettings::parse(text);
        EXPECT_EQ(settings.max_nights, 12) << text;
        EXPECT_EQ(settings.updates, UpdateOptions{}) << text;
    }
    // The last of duplicate names wins, also when it names no value.
    EXPECT_EQ(AppSettings::parse(R"({"UpdateCheckFrequency": "Monthly", "UpdateCheckFrequency": "Yearly"})")
                  .updates.check_frequency,
              CheckFrequency::daily);
}

TEST(Settings, Update_option_values_ignore_ascii_case_but_names_do_not)
{
    const AppSettings settings =
        AppSettings::parse(R"({"UpdateCheckFrequency": "weekly", "WhenUpdateFound": "NOTIFYONLY"})");
    EXPECT_EQ(settings.updates.check_frequency, CheckFrequency::weekly);
    EXPECT_EQ(settings.updates.when_found, WhenFound::notify_only);
    EXPECT_EQ(AppSettings::parse(R"({"UpdateCheckFrequency": "mOnThLy"})").updates.check_frequency,
              CheckFrequency::monthly);
    EXPECT_EQ(parse_when_found("downloadthenask"), WhenFound::download_then_ask);
    EXPECT_EQ(parse_check_frequency("Fortnightly"), std::nullopt);
    EXPECT_EQ(json_name(CheckFrequency::weekly), "Weekly");
    EXPECT_EQ(json_name(WhenFound::notify_only), "NotifyOnly");
    // Property names stay case-sensitive: these are unknown properties, ignored.
    EXPECT_EQ(AppSettings::parse(R"({"checkForUpdates": false, "updateCheckFrequency": "Monthly"})"), AppSettings{});
}

TEST(Settings, A_wrong_type_for_an_update_option_gives_all_defaults)
{
    for (const std::string_view bad : {
             R"({"MaxNights": 12, "CheckForUpdates": "false"})",
             R"({"MaxNights": 12, "CheckForUpdates": 0})",
             R"({"MaxNights": 12, "CheckForUpdates": null})",
             R"({"MaxNights": 12, "UpdateCheckFrequency": 1})",
             R"({"MaxNights": 12, "UpdateCheckFrequency": null})",
             R"({"MaxNights": 12, "UpdateCheckFrequency": ["Weekly"]})",
             R"({"MaxNights": 12, "WhenUpdateFound": true})",
             R"({"MaxNights": 12, "WhenUpdateFound": {"Value": "NotifyOnly"}})",
         }) {
        EXPECT_EQ(AppSettings::parse(bad), AppSettings{}) << bad;
    }
}

TEST(Settings, A_file_of_2_2_reads_the_reuse_window_as_its_default)
{
    AppSettings expected;
    expected.window_width = 1320;
    expected.window_height = 820;
    const AppSettings settings = AppSettings::parse(kExampleOf22);
    EXPECT_EQ(settings, expected);
    EXPECT_EQ(settings.fare_reuse, ReuseWindow::thirty_minutes);
}

TEST(Settings, The_reuse_window_is_written_after_the_update_options_with_its_json_name_and_round_trips)
{
    const std::pair<ReuseWindow, std::string_view> cases[] = {
        {ReuseWindow::thirty_minutes, "ThirtyMinutes"},
        {ReuseWindow::one_hour, "OneHour"},
        {ReuseWindow::three_hours, "ThreeHours"},
        {ReuseWindow::six_hours, "SixHours"},
    };
    for (const auto& [window, name] : cases) {
        AppSettings settings;
        settings.fare_reuse = window;
        const std::string text = settings.to_json();
        EXPECT_TRUE(text.ends_with("  \"WhenUpdateFound\": \"InstallOnClose\",\r\n  \"ReuseFaresWithin\": \"" +
                                   std::string{name} + "\",\r\n  \"HomesWithin\": \"FiveKm\"\r\n}"))
            << text;
        EXPECT_EQ(AppSettings::parse(text), settings);
        EXPECT_EQ(json_name(window), name);
        EXPECT_EQ(parse_reuse_window(name), window);
    }
}

TEST(Settings, Reuse_window_values_ignore_ascii_case_but_the_name_does_not)
{
    EXPECT_EQ(AppSettings::parse(R"({"ReuseFaresWithin": "onehour"})").fare_reuse, ReuseWindow::one_hour);
    EXPECT_EQ(AppSettings::parse(R"({"ReuseFaresWithin": "SIXHOURS"})").fare_reuse, ReuseWindow::six_hours);
    EXPECT_EQ(AppSettings::parse(R"({"ReuseFaresWithin": "threeHours"})").fare_reuse, ReuseWindow::three_hours);
    EXPECT_EQ(parse_reuse_window("thirtyminutes"), ReuseWindow::thirty_minutes);
    // The property name is case-sensitive: an unknown property, ignored.
    EXPECT_EQ(AppSettings::parse(R"({"reuseFaresWithin": "SixHours"})"), AppSettings{});
}

TEST(Settings, An_unknown_reuse_window_gives_that_property_its_default_only)
{
    for (const std::string_view unknown : {"TwoHours", "", " OneHour", "OneHour ", "1 hour", "30"}) {
        const std::string text = R"({"MaxNights": 12, "CheckForUpdates": false, "UpdateCheckFrequency": "Weekly", )"
                                 R"("ReuseFaresWithin": ")" + std::string{unknown} + R"("})";
        const AppSettings settings = AppSettings::parse(text);
        EXPECT_EQ(settings.max_nights, 12) << text;
        EXPECT_FALSE(settings.updates.check_automatically) << text;
        EXPECT_EQ(settings.updates.check_frequency, CheckFrequency::weekly) << text;
        EXPECT_EQ(settings.fare_reuse, ReuseWindow::thirty_minutes) << text;
    }
    EXPECT_EQ(parse_reuse_window("TwoHours"), std::nullopt);
}

TEST(Settings, A_wrong_type_for_the_reuse_window_gives_all_defaults)
{
    for (const std::string_view bad : {
             R"({"MaxNights": 12, "ReuseFaresWithin": 60})",
             R"({"MaxNights": 12, "ReuseFaresWithin": null})",
             R"({"MaxNights": 12, "ReuseFaresWithin": true})",
             R"({"MaxNights": 12, "ReuseFaresWithin": ["OneHour"]})",
             R"({"MaxNights": 12, "ReuseFaresWithin": {"Value": "OneHour"}})",
         }) {
        EXPECT_EQ(AppSettings::parse(bad), AppSettings{}) << bad;
    }
}

TEST(Settings, A_file_with_the_reuse_window_read_by_the_rules_of_2_1_keeps_its_twelve_values)
{
    AppSettings settings;
    settings.from = "Wien";
    settings.adults = 2;
    settings.max_nights = 7;
    settings.fare_reuse = ReuseWindow::six_hours;
    const std::optional<Settings21> old = read_like_2_1(settings.to_json());
    ASSERT_TRUE(old.has_value()) << "2.1 would fall back to all defaults";
    EXPECT_EQ(old->from, "Wien");
    EXPECT_EQ(old->adults, 2);
    EXPECT_EQ(old->max_nights, 7);
}

// --- HomesWithin (D-73) ----------------------------------------------------------------

TEST(Settings, D_73_Homes_within_is_written_last_with_its_json_name_and_round_trips)
{
    const std::pair<HomesRadius, std::string_view> cases[] = {
        {HomesRadius::two_km, "TwoKm"},
        {HomesRadius::five_km, "FiveKm"},
        {HomesRadius::ten_km, "TenKm"},
        {HomesRadius::twenty_five_km, "TwentyFiveKm"},
    };
    for (const auto& [radius, name] : cases) {
        AppSettings settings;
        settings.homes_within = radius;
        const std::string text = settings.to_json();
        EXPECT_TRUE(text.ends_with("  \"ReuseFaresWithin\": \"ThirtyMinutes\",\r\n  \"HomesWithin\": \"" +
                                   std::string{name} + "\"\r\n}"))
            << text;
        EXPECT_EQ(AppSettings::parse(text), settings);
        EXPECT_EQ(json_name(radius), name);
        EXPECT_EQ(parse_homes_radius(name), radius);
    }
    EXPECT_EQ(AppSettings{}.homes_within, HomesRadius::five_km) << "missing: 5 km";
    EXPECT_EQ(AppSettings::parse(R"({"MaxNights": 12})").homes_within, HomesRadius::five_km);
}

TEST(Settings, D_73_Homes_within_values_ignore_ascii_case_but_the_name_does_not)
{
    EXPECT_EQ(AppSettings::parse(R"({"HomesWithin": "tenkm"})").homes_within, HomesRadius::ten_km);
    EXPECT_EQ(AppSettings::parse(R"({"HomesWithin": "TWOKM"})").homes_within, HomesRadius::two_km);
    EXPECT_EQ(parse_homes_radius("twentyFiveKM"), HomesRadius::twenty_five_km);
    EXPECT_EQ(AppSettings::parse(R"({"homesWithin": "TwoKm"})"), AppSettings{}) << "an unknown property";
}

TEST(Settings, D_73_An_unknown_homes_distance_gives_that_property_its_default_only)
{
    for (const std::string_view unknown : {"FiftyKm", "", " TwoKm", "2 km", "2"}) {
        const std::string text = R"({"MaxNights": 12, "ReuseFaresWithin": "OneHour", "HomesWithin": ")" +
                                 std::string{unknown} + R"("})";
        const AppSettings settings = AppSettings::parse(text);
        EXPECT_EQ(settings.max_nights, 12) << text;
        EXPECT_EQ(settings.fare_reuse, ReuseWindow::one_hour) << text;
        EXPECT_EQ(settings.homes_within, HomesRadius::five_km) << text;
    }
    EXPECT_EQ(parse_homes_radius("FiftyKm"), std::nullopt);
}

TEST(Settings, D_73_A_wrong_type_for_the_homes_distance_gives_all_defaults)
{
    for (const std::string_view bad : {
             R"({"MaxNights": 12, "HomesWithin": 5})",
             R"({"MaxNights": 12, "HomesWithin": null})",
             R"({"MaxNights": 12, "HomesWithin": false})",
             R"({"MaxNights": 12, "HomesWithin": ["TwoKm"]})",
         }) {
        EXPECT_EQ(AppSettings::parse(bad), AppSettings{}) << bad;
    }
}

TEST(Settings, D_73_A_file_with_the_homes_distance_read_by_the_rules_of_2_1_keeps_its_twelve_values)
{
    AppSettings settings;
    settings.from = "Wien";
    settings.max_nights = 7;
    settings.homes_within = HomesRadius::twenty_five_km;
    const std::optional<Settings21> old = read_like_2_1(settings.to_json());
    ASSERT_TRUE(old.has_value()) << "2.1 would fall back to all defaults";
    EXPECT_EQ(old->from, "Wien");
    EXPECT_EQ(old->max_nights, 7);
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
    settings.updates = UpdateOptions{.check_automatically = false,
                                     .check_frequency = CheckFrequency::monthly,
                                     .when_found = WhenFound::notify_only};
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
