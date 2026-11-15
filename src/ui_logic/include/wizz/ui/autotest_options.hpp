#pragma once
// The hidden self-test command line (01 §17.1, `AutoTestOptions.Parse`) plus
// the test-only companions of DECISIONS D-30.
//
//   WizzairHelper.exe --autotest <outdir> [--from X] [--to Y] [--from-radius N] [--to-radius N]
//                     [--out A:B] [--ret A:B] [--currency C] [--then-currency C]
//                     [--return-from-top] [--keep-open]
//                     [--data-root <dir>] [--replay <dir>] [--fixed-now <yyyy-MM-ddTHH:mm:ssZ>]
//                     [--no-pacing] [--warm-cache] [--cancel-after-ms <n>] [--diagnostics]       (D-30)
//
// Flag matching is exact and case-sensitive; the FIRST occurrence of a flag
// wins and its value is the next argument whatever it looks like (.NET
// Array.IndexOf); a flag in the last position has no value. Empty values are
// kept (`--to ""` = anywhere).
//
// D-30 companions (hidden, only together with --autotest):
//   --data-root <dir>   replaces %LOCALAPPDATA%\WizzairHelper as the root of
//                       settings.json, errors.log and cache\ (isolated profile);
//   --replay <dir>      serves every Wizz / FX / geocoder HTTP request from the
//                       fixture directory <dir> through ReplayTransport
//                       (replay_transport.hpp documents the format);
//   --fixed-now <utc>   runs on a FixedClock at that UTC instant, local zone UTC
//                       ("today" = its date), e.g. 2026-11-02T10:00:00Z;
//   --no-pacing         no 1 s request spacing / jitter in front of Wizz requests
//                       (a replay talks to no server; retry back-offs stay);
//   --warm-cache        runs the outbound search once before the script, so the
//                       script's outbound search replays cached timetables (05 §5
//                       "cached replay");
//   --cancel-after-ms n presses Esc and Cancel n ms after the script's outbound
//                       search starts fetching (05 §5 cancellation checks);
//   --diagnostics       writes diagnostics.json next to summary.json (40 ms timer
//                       gaps, cancel transitions, final grids, display, layout).
// Relative directories are made absolute against the current directory, like
// the output directory.

#include "wizz/core/locale.hpp"
#include "wizz/core/time.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace wizz::ui {

/// An inclusive `--out` / `--ret` date window.
struct AutoTestDates {
    Date start;
    Date end;
    friend constexpr bool operator==(const AutoTestDates&, const AutoTestDates&) = default;
};

struct AutoTestOptions {
    std::filesystem::path output_dir;             ///< --autotest, made absolute
    std::string from{"Budapest"};                 ///< --from
    std::string to{"London"};                     ///< --to ("" = anywhere)
    std::optional<int> from_radius_km;            ///< --from-radius, clamped 0..1500; nullopt = settings value
    std::optional<int> to_radius_km;              ///< --to-radius, clamped 0..1500
    std::optional<AutoTestDates> outbound;        ///< --out; nullopt = the normal defaults
    std::optional<AutoTestDates> inbound;         ///< --ret
    std::optional<std::string> currency;          ///< --currency as given (applied only when listed)
    std::optional<std::string> then_currency;     ///< --then-currency as given (applied only when listed)
    bool return_from_top = false;                 ///< --return-from-top
    bool keep_open = false;                       ///< --keep-open
    std::optional<std::filesystem::path> data_root;  ///< --data-root (D-30)
    std::optional<std::filesystem::path> replay_dir; ///< --replay (D-30)
    std::optional<UtcInstant> fixed_now;             ///< --fixed-now (D-30)
    bool no_pacing = false;                          ///< --no-pacing (D-30)
    bool warm_cache = false;                         ///< --warm-cache (D-30)
    std::optional<std::chrono::milliseconds> cancel_after;  ///< --cancel-after-ms (D-30)
    bool diagnostics = false;                        ///< --diagnostics (D-30)

    /// Parses `args` — the command-line arguments as UTF-8, WITHOUT the program path.
    /// Returns nullopt for a normal run (no `--autotest`, or no value after it).
    /// `number_symbols` are the current culture's (int.TryParse of the radii).
    /// Throws wizz::FormatError for a bad `--out` / `--ret` date (yyyy-MM-dd, invariant),
    /// `--fixed-now` instant or `--cancel-after-ms` value, and std::invalid_argument for an empty directory
    /// value (Path.GetFullPath("")) — the app then crashes before its window opens.
    [[nodiscard]] static std::optional<AutoTestOptions> parse(std::span<const std::string> args,
                                                              const std::filesystem::path& current_dir,
                                                              const NumberSymbols& number_symbols = NumberSymbols{});

    /// The `--data-root` of an --autotest command line, made absolute, read on its own
    /// with the rules of parse(): a start-up crash over another bad value still belongs
    /// in that isolated profile's errors.log (D-30). nullopt for a normal run and when
    /// there is no `--data-root` value or it is empty.
    [[nodiscard]] static std::optional<std::filesystem::path> parse_data_root(std::span<const std::string> args,
                                                                             const std::filesystem::path& current_dir);

    /// `--currency` when it is in the "Prices in" list (exact), else nullopt.
    [[nodiscard]] std::optional<std::string> listed_currency() const;
    /// `--then-currency` when it is listed, else nullopt.
    [[nodiscard]] std::optional<std::string> listed_then_currency() const;
    /// The script runs a return search when `--ret` or `--return-from-top` is given.
    [[nodiscard]] bool runs_return() const noexcept { return inbound.has_value() || return_from_top; }
};

/// .NET `int.TryParse(text, out n)` (NumberStyles.Integer, current culture): optional
/// leading/trailing white space (U+0009–U+000D, U+0020), an optional leading sign (the
/// culture's negative sign, "+", or "-" when the negative sign is a minus-like character),
/// ASCII digits, Int32 range. nullopt when it does not parse.
[[nodiscard]] std::optional<int> parse_culture_int(std::string_view text, const NumberSymbols& symbols) noexcept;

/// The `--out` / `--ret` value: split on ':', start = the first part, end = the LAST part
/// (a single date gives start = end), each `yyyy-MM-dd` (DateOnly.ParseExact, invariant).
/// Throws wizz::FormatError naming the bad part.
[[nodiscard]] AutoTestDates parse_autotest_dates(std::string_view value);

/// `--fixed-now`: `yyyy-MM-ddTHH:mm:ss[.fffffff][Z]` read as UTC. Throws wizz::FormatError.
[[nodiscard]] UtcInstant parse_fixed_now(std::string_view value);

/// `--cancel-after-ms`: 1 to 9 ASCII digits (invariant), 0 allowed. Throws wizz::FormatError.
[[nodiscard]] std::chrono::milliseconds parse_cancel_after(std::string_view value);

} // namespace wizz::ui
