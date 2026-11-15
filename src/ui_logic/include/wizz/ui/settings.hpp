#pragma once
// settings.json persistence (01 §13, 06 §3) and the way the input panel
// applies it (01 §4.1, §4.4).
//
// Load follows System.Text.Json defaults for the .NET AppSettings class:
// property names are case-sensitive, missing ones keep their defaults, unknown
// ones are ignored, a `null` root gives defaults, the last of duplicate names
// wins, and a JSON `null` for a string property gives an empty text. ANY I/O
// error, malformed JSON (comments and trailing commas included), non-object
// root or wrong type — "two" or 250.5 or 250.0 or 1e2 or null for an int, a
// non-boolean for a bool, a non-string for a string — falls back to ALL
// defaults. The reader strips a byte-order mark and decodes \u escapes,
// surrogate pairs included. No clamping happens here; `initial_inputs`
// applies the UI's clamps.
//
// Save writes the twelve properties in table order, indented by two spaces,
// into a complete temporary file in the target directory and then replaces the
// target (last complete write wins); I/O errors are swallowed.

#include <filesystem>
#include <string>
#include <string_view>

namespace wizz::ui {

struct AppSettings {
    std::string from{"Budapest"};
    std::string to{"London"};
    int from_radius_km = 250;
    int to_radius_km = 100;
    int adults = 1;
    bool discount_club = false;
    std::string currency{"EUR"};
    int min_nights = 2;
    int max_nights = 10;
    int window_width = 0;   ///< device px; > 0 = restore the saved size
    int window_height = 0;  ///< device px
    bool maximized = false;

    friend bool operator==(const AppSettings&, const AppSettings&) = default;

    /// Settings from JSON text (see the file comment). Never throws.
    [[nodiscard]] static AppSettings parse(std::string_view json_text) noexcept;
    /// `AppSettings.Load`: defaults when the file does not exist or cannot be read,
    /// otherwise `parse` of its text (BOM-detected like File.ReadAllText). Never throws.
    [[nodiscard]] static AppSettings load(const std::filesystem::path& path) noexcept;

    /// The settings.json text: an object with From, To, FromRadiusKm, ToRadiusKm, Adults,
    /// DiscountClub, Currency, MinNights, MaxNights, WindowWidth, WindowHeight, Maximized
    /// in that order, indented two spaces (CRLF line breaks, JsonSerializer escaping).
    [[nodiscard]] std::string to_json() const;
    /// Writes `to_json()` to `path`: creates the directory, writes `<name>.tmp` next to
    /// it, then replaces the target atomically. Returns false on any error (the caller
    /// ignores it: "ignore IO/Unauthorized errors").
    bool save(const std::filesystem::path& path) const noexcept;
};

/// The input-panel start values derived from settings (01 §4.4).
struct InitialInputs {
    std::string from;           ///< `_from` text
    std::string to;             ///< `_to` text (empty = anywhere)
    int from_radius_km = 250;   ///< clamp 0..1500
    int to_radius_km = 100;     ///< clamp 0..1500
    int adults = 1;             ///< clamp 1..9
    int fare_index = 0;         ///< 1 when DiscountClub
    std::string currency;       ///< the setting when listed (exact, case-sensitive), else "EUR"
    int min_nights = 2;         ///< clamp 0..60
    int max_nights = 10;        ///< clamp 0..120, then ≥ min_nights

    friend bool operator==(const InitialInputs&, const InitialInputs&) = default;
};
[[nodiscard]] InitialInputs initial_inputs(const AppSettings& settings);

/// Radius editors range 0..1500 km (01 §4.4).
inline constexpr int kMaxRadiusKm = 1500;

/// Numeric editors (01 §4.4 note): they display whole numbers rounded half away from
/// zero but keep the typed fraction; radii search with the fraction, while adults,
/// night filters and persisted integers truncate toward zero (2.7 adults shows "3",
/// searches 2).
[[nodiscard]] double numeric_display_value(double value) noexcept;
[[nodiscard]] int truncate_toward_zero(double value) noexcept;

/// Window size (01 §4.1, UX-15).
struct WindowSize {
    int width = 0;
    int height = 0;
    friend bool operator==(const WindowSize&, const WindowSize&) = default;
};
/// The start size: `fallback` (L(1320) × L(820)) when `settings.window_width <= 0`,
/// otherwise the saved device-pixel size; then W = clamp(w, MinW, max(MinW, work.W)) and
/// H = clamp(h, MinH, max(MinH, work.H)) for the work area of the cursor's screen.
[[nodiscard]] WindowSize start_window_size(const AppSettings& settings, WindowSize fallback, WindowSize minimum,
                                           WindowSize work_area) noexcept;
/// Maximised at start when the setting says so and not in autotest.
[[nodiscard]] constexpr bool start_maximized(const AppSettings& settings, bool autotest) noexcept
{
    return settings.maximized && !autotest;
}

} // namespace wizz::ui
