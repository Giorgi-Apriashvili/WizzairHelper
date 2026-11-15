#pragma once
// Numeric label rounding (07 §2) and the busy-panel / status duration texts
// (01 §10.7, §11.10, DECISIONS D-17). Money is formatted elsewhere (03 §6):
// never pass money through these helpers.

#include "wizz/core/locale.hpp"
#include "wizz/core/time.hpp"

#include <string>

namespace wizz {

/// A distance or duration label as .NET custom formats `"0"` (decimals = 0) and
/// `"0.0"` (decimals = 1) print a double (07 §2, 03 §0):
/// 1. the binary value is rounded to 15 significant decimal digits;
/// 2. that decimal is rounded to `decimals` places, exact halves away from zero;
/// 3. no group separator; the culture's decimal separator and negative sign.
/// Examples: 172.4999999999999 → "173" / "172.5"; 4.35 → "4" / "4.4"; 0.25 → "0" / "0.3".
/// A value that rounds to zero prints without a sign. NaN prints "NaN", infinities "∞" / "-∞".
/// `decimals` must be in [0, 15] (std::invalid_argument otherwise).
[[nodiscard]] std::string format_label(double value, int decimals, const NumberSymbols& symbols);

/// Round to nearest, ties to even, on the original binary value (07 §2: grid
/// distances, whole-second arithmetic, longitude offsets). Same result as
/// std::nearbyint under the default rounding mode, independent of the FP environment.
[[nodiscard]] double round_half_even(double value) noexcept;

/// `FormatDuration` (01 §11.10, D-17): "{whole minutes} min {seconds component} s"
/// from one minute up ("1 min 19 s", "11 min 5 s", seconds not padded); below
/// that "{max(1, round-half-even(total seconds))} s" ("59 s", "2 s" for 2.5 s, "1 s" for 0.2 s).
[[nodiscard]] std::string format_duration(Duration duration);

/// `FormatElapsed` (01 §10.7): TimeSpan custom format "h:mm:ss" from one hour up,
/// else "m:ss" ("0:08", "1:05", "1:02:05"). Components are truncated; like .NET the
/// hour field is the 0–23 hours component and a negative span prints its magnitude.
[[nodiscard]] std::string format_elapsed(Duration duration);

/// `FormatRemaining` (01 §10.7): below 60 s "{max(1, ceil(s))} s" ("25 s");
/// otherwise "{(int)(s / 60)} min {two-digit seconds component} s" ("2 min 05 s").
[[nodiscard]] std::string format_remaining(double seconds);

} // namespace wizz
