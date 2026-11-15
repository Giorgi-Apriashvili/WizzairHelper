#pragma once
// Naive wall-clock date-times as Wizz sends them (route-map `operationStartDate`,
// timetable `departureDate` / `departureDates`) and their invariant text forms
// (02 §2.4, 04 §9.4, 07 §1, DECISIONS D-22).

#include "wizz/core/time.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace wizz {

/// Parses a timetable / route-map date-time as a naive local wall-clock value
/// (the C++ stand-in for `DateTime.TryParse(s, InvariantCulture)` + Unspecified).
///
/// Accepted (D-22), with optional surrounding ASCII white space:
/// - `yyyy-MM-dd` (midnight);
/// - `yyyy-MM-ddTHH:mm`, `yyyy-MM-ddTHH:mm:ss`, `yyyy-MM-ddTHH:mm:ss.f` … `.fffffff`
///   (a single space may replace the `T`);
/// - the same date-times followed by `Z` or an offset `±HH`, `±HHmm`, `±HH:mm`,
///   which is **ignored**: the wall clock is kept, never converted;
/// - invariant `MM/dd/yyyy` (midnight).
///
/// Calendar-invalid values (`2026-02-30`, `24:00:00`, minute or second 60, year 0)
/// and every other shape give nullopt. `1900-01-01T00:00:00` (Wizz's "nothing on
/// sale" marker) parses normally; interpreting it is the planner's job.
[[nodiscard]] std::optional<LocalDateTime> parse_local_datetime(std::string_view text);

/// Invariant `HH:mm` (07 §1: time cells, `CheapestDeparture:HH:mm` in logs).
[[nodiscard]] std::string format_hhmm(LocalDateTime time);

/// Invariant `yyyy-MM-ddTHH:mm:ss` (.NET "s": error log, summary.json `startedAt`).
/// Any sub-second part is truncated.
[[nodiscard]] std::string format_iso_seconds(LocalDateTime time);

/// ISO 8601 UTC text for logs and test diagnostics: `yyyy-MM-ddTHH:mm:ssZ`, or
/// `yyyy-MM-ddTHH:mm:ss.fffffffZ` when the instant has a sub-second part.
[[nodiscard]] std::string format_iso_utc(UtcInstant instant);

} // namespace wizz
