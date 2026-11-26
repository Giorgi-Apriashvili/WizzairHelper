#include "wizz/ui/settings.hpp"

#include "wizz/ui/strings.hpp"

#include "wizz/core/json.hpp"
#include "wizz/core/text_decoding.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <system_error>

namespace wizz::ui {

namespace {

/// A property of the wrong JSON kind: System.Text.Json throws JsonException, so the
/// whole file falls back to defaults.
struct WrongType {};

[[nodiscard]] std::string read_string(const json::Value& value)
{
    if (value.is_null()) {
        return {};  // null string → null → shown as empty text
    }
    if (!value.is_string()) {
        throw WrongType{};
    }
    return std::string{value.as_string()};
}

[[nodiscard]] int read_int(const json::Value& value)
{
    const std::optional<std::int32_t> number = value.try_int32();
    if (!number) {
        throw WrongType{};  // "two", 250.5, 250.0, 1e2, out of range, null
    }
    return *number;
}

[[nodiscard]] bool read_bool(const json::Value& value)
{
    if (!value.is_bool()) {
        throw WrongType{};
    }
    return value.as_bool();
}

/// An enumeration (D-60, D-68, D-73, D-75): a non-string (null included) is a wrong type
/// like anywhere else; a string naming no value gives that property's default only.
template <class Enum>
[[nodiscard]] Enum read_enum(const json::Value& value, std::optional<Enum> (*parse)(std::string_view) noexcept,
                             Enum fallback)
{
    if (!value.is_string()) {
        throw WrongType{};
    }
    return parse(value.as_string()).value_or(fallback);
}

[[nodiscard]] constexpr bool equals_ascii_ignoring_case(std::string_view a, std::string_view b) noexcept
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() && std::ranges::equal(a, b, {}, lower, lower);
}

constexpr update::CheckFrequency kFrequencies[] = {update::CheckFrequency::daily, update::CheckFrequency::weekly,
                                                   update::CheckFrequency::monthly};
constexpr update::WhenFound kWhenFound[] = {update::WhenFound::install_on_close, update::WhenFound::download_then_ask,
                                            update::WhenFound::notify_only};
constexpr fare_days::ReuseWindow kReuseWindows[] = {fare_days::ReuseWindow::thirty_minutes,
                                                    fare_days::ReuseWindow::one_hour,
                                                    fare_days::ReuseWindow::three_hours,
                                                    fare_days::ReuseWindow::six_hours};

void apply_member(AppSettings& settings, const json::Member& member)
{
    const std::string_view name = member.name();
    const json::Value& value = member.value();
    if (name == "From") {
        settings.from = read_string(value);
    } else if (name == "To") {
        settings.to = read_string(value);
    } else if (name == "FromRadiusKm") {
        settings.from_radius_km = read_int(value);
    } else if (name == "ToRadiusKm") {
        settings.to_radius_km = read_int(value);
    } else if (name == "Adults") {
        settings.adults = read_int(value);
    } else if (name == "DiscountClub") {
        settings.discount_club = read_bool(value);
    } else if (name == "Currency") {
        settings.currency = read_string(value);
    } else if (name == "MinNights") {
        settings.min_nights = read_int(value);
    } else if (name == "MaxNights") {
        settings.max_nights = read_int(value);
    } else if (name == "WindowWidth") {
        settings.window_width = read_int(value);
    } else if (name == "WindowHeight") {
        settings.window_height = read_int(value);
    } else if (name == "Maximized") {
        settings.maximized = read_bool(value);
    } else if (name == "CheckForUpdates") {
        settings.updates.check_automatically = read_bool(value);
    } else if (name == "UpdateCheckFrequency") {
        settings.updates.check_frequency =
            read_enum(value, &parse_check_frequency, update::UpdateOptions{}.check_frequency);
    } else if (name == "WhenUpdateFound") {
        settings.updates.when_found = read_enum(value, &parse_when_found, update::UpdateOptions{}.when_found);
    } else if (name == "ReuseFaresWithin") {
        settings.fare_reuse = read_enum(value, &parse_reuse_window, AppSettings{}.fare_reuse);
    } else if (name == "HomesWithin") {
        settings.homes_within = read_enum(value, &parse_homes_radius, AppSettings{}.homes_within);
    } else if (name == "Theme") {
        settings.theme = read_enum(value, &parse_theme_choice, AppSettings{}.theme);
    }
    // unknown properties are ignored
}

[[nodiscard]] std::optional<std::string> read_file_bytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    if (in.bad()) {
        return std::nullopt;
    }
    return bytes;
}

} // namespace

std::string_view json_name(update::CheckFrequency frequency) noexcept
{
    switch (frequency) {
    case update::CheckFrequency::daily:
        return "Daily";
    case update::CheckFrequency::weekly:
        return "Weekly";
    case update::CheckFrequency::monthly:
        return "Monthly";
    }
    return "Daily";
}

std::string_view json_name(update::WhenFound when_found) noexcept
{
    switch (when_found) {
    case update::WhenFound::install_on_close:
        return "InstallOnClose";
    case update::WhenFound::download_then_ask:
        return "DownloadThenAsk";
    case update::WhenFound::notify_only:
        return "NotifyOnly";
    }
    return "InstallOnClose";
}

std::string_view json_name(fare_days::ReuseWindow window) noexcept
{
    switch (window) {
    case fare_days::ReuseWindow::thirty_minutes:
        return "ThirtyMinutes";
    case fare_days::ReuseWindow::one_hour:
        return "OneHour";
    case fare_days::ReuseWindow::three_hours:
        return "ThreeHours";
    case fare_days::ReuseWindow::six_hours:
        return "SixHours";
    }
    return "ThirtyMinutes";
}

std::string_view json_name(HomesRadius radius) noexcept
{
    switch (radius) {
    case HomesRadius::two_km:
        return "TwoKm";
    case HomesRadius::five_km:
        return "FiveKm";
    case HomesRadius::ten_km:
        return "TenKm";
    case HomesRadius::twenty_five_km:
        return "TwentyFiveKm";
    }
    return "FiveKm";
}

std::string_view json_name(ThemeChoice choice) noexcept
{
    switch (choice) {
    case ThemeChoice::automatic:
        return "Auto";
    case ThemeChoice::dark:
        return "Dark";
    case ThemeChoice::light:
        return "Light";
    }
    return "Auto";
}

std::optional<update::CheckFrequency> parse_check_frequency(std::string_view text) noexcept
{
    for (const update::CheckFrequency frequency : kFrequencies) {
        if (equals_ascii_ignoring_case(text, json_name(frequency))) {
            return frequency;
        }
    }
    return std::nullopt;
}

std::optional<update::WhenFound> parse_when_found(std::string_view text) noexcept
{
    for (const update::WhenFound when_found : kWhenFound) {
        if (equals_ascii_ignoring_case(text, json_name(when_found))) {
            return when_found;
        }
    }
    return std::nullopt;
}

std::optional<fare_days::ReuseWindow> parse_reuse_window(std::string_view text) noexcept
{
    for (const fare_days::ReuseWindow window : kReuseWindows) {
        if (equals_ascii_ignoring_case(text, json_name(window))) {
            return window;
        }
    }
    return std::nullopt;
}

std::optional<HomesRadius> parse_homes_radius(std::string_view text) noexcept
{
    for (const HomesRadius radius : kHomesRadii) {
        if (equals_ascii_ignoring_case(text, json_name(radius))) {
            return radius;
        }
    }
    return std::nullopt;
}

std::optional<ThemeChoice> parse_theme_choice(std::string_view text) noexcept
{
    for (const ThemeChoice choice : kThemeChoices) {
        if (equals_ascii_ignoring_case(text, json_name(choice))) {
            return choice;
        }
    }
    return std::nullopt;
}

AppSettings AppSettings::parse(std::string_view json_text) noexcept
{
    try {
        const json::Value root = json::parse(json_text);
        AppSettings settings;
        if (root.is_null()) {
            return settings;
        }
        if (!root.is_object()) {
            return AppSettings{};
        }
        for (const json::Member& member : root.as_object()) {
            apply_member(settings, member);  // source order: the last duplicate wins
        }
        return settings;
    } catch (...) {
        return AppSettings{};  // JsonException / wrong type: all defaults
    }
}

AppSettings AppSettings::load(const std::filesystem::path& path) noexcept
{
    try {
        std::error_code error;
        if (!std::filesystem::exists(path, error) || error) {
            return AppSettings{};
        }
        const std::optional<std::string> bytes = read_file_bytes(path);
        if (!bytes) {
            return AppSettings{};
        }
        return parse(decode_text(*bytes));
    } catch (...) {
        return AppSettings{};
    }
}

std::string AppSettings::to_json() const
{
    json::Writer writer{json::Layout::indented};
    writer.begin_object()
        .key("From").string(from)
        .key("To").string(to)
        .key("FromRadiusKm").integer(from_radius_km)
        .key("ToRadiusKm").integer(to_radius_km)
        .key("Adults").integer(adults)
        .key("DiscountClub").boolean(discount_club)
        .key("Currency").string(currency)
        .key("MinNights").integer(min_nights)
        .key("MaxNights").integer(max_nights)
        .key("WindowWidth").integer(window_width)
        .key("WindowHeight").integer(window_height)
        .key("Maximized").boolean(maximized)
        .key("CheckForUpdates").boolean(updates.check_automatically)
        .key("UpdateCheckFrequency").string(json_name(updates.check_frequency))
        .key("WhenUpdateFound").string(json_name(updates.when_found))
        .key("ReuseFaresWithin").string(json_name(fare_reuse))
        .key("HomesWithin").string(json_name(homes_within))
        .key("Theme").string(json_name(theme))
        .end_object();
    return writer.take();
}

bool AppSettings::save(const std::filesystem::path& path) const noexcept
{
    try {
        const std::string text = to_json();
        std::error_code error;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), error);
            if (error) {
                return false;
            }
        }
        std::filesystem::path temp = path;
        temp += L"." + std::to_wstring(::GetCurrentProcessId()) + L".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return false;
            }
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out) {
                out.close();
                std::filesystem::remove(temp, error);
                return false;
            }
        }
        if (::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
            std::filesystem::remove(temp, error);
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

InitialInputs initial_inputs(const AppSettings& settings)
{
    InitialInputs inputs;
    inputs.from = settings.from;
    inputs.to = settings.to;
    inputs.from_radius_km = std::clamp(settings.from_radius_km, 0, kMaxRadiusKm);
    inputs.to_radius_km = std::clamp(settings.to_radius_km, 0, kMaxRadiusKm);
    inputs.adults = std::clamp(settings.adults, 1, 9);
    inputs.fare_index = settings.discount_club ? 1 : 0;
    inputs.currency = strings::is_listed_currency(settings.currency) ? settings.currency : std::string{"EUR"};
    inputs.min_nights = std::clamp(settings.min_nights, 0, 60);
    inputs.max_nights = std::max(std::clamp(settings.max_nights, 0, 120), inputs.min_nights);
    return inputs;
}

double numeric_display_value(double value) noexcept
{
    return std::round(value);  // half away from zero
}

int truncate_toward_zero(double value) noexcept
{
    if (std::isnan(value)) {
        return 0;
    }
    const double truncated = std::trunc(value);
    if (truncated >= static_cast<double>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    if (truncated <= static_cast<double>(std::numeric_limits<int>::min())) {
        return std::numeric_limits<int>::min();
    }
    return static_cast<int>(truncated);
}

WindowSize start_window_size(const AppSettings& settings, WindowSize fallback, WindowSize minimum,
                             WindowSize work_area) noexcept
{
    const WindowSize wanted =
        settings.window_width <= 0 ? fallback : WindowSize{settings.window_width, settings.window_height};
    return WindowSize{std::clamp(wanted.width, minimum.width, std::max(minimum.width, work_area.width)),
                      std::clamp(wanted.height, minimum.height, std::max(minimum.height, work_area.height))};
}

} // namespace wizz::ui
