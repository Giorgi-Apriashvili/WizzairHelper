#pragma once
// BookingLinks (03 §9, protocol-reference §8): deep links into the Wizz Air
// booking SPA. Codes are upper-cased (ToUpperInvariant), dates are invariant
// `yyyy-MM-dd` (07 §1), adults are clamped to at least 1 (CT-LNK-01, P-29).
// Callers pass the fare row's real station codes, never a queried group code.

#include "wizz/core/time.hpp"

#include <string>
#include <string_view>

namespace wizz::booking_links {

/// `Base` of every link (03 §9).
inline constexpr std::string_view kBase = "https://www.wizzair.com/en-gb/booking/select-flight";

/// OneWay: `{Base}/{FROM}/{TO}/{yyyy-MM-dd}/null/{max(1,adults)}/0/0/null`, e.g.
/// `https://www.wizzair.com/en-gb/booking/select-flight/BTS/LTN/2026-11-12/null/2/0/0/null`.
[[nodiscard]] std::string one_way(std::string_view from, std::string_view to, Date date, int adults);

/// Return (03 §9; used for RoundTrip.CombinedBookingUrl, 03 §10.1):
/// `{Base}/{FROM}/{TO}/{outDate:yyyy-MM-dd}/{returnDate:yyyy-MM-dd}/{max(1,adults)}/0/0/null`.
/// Named `round_trip` because `return` is a keyword.
[[nodiscard]] std::string round_trip(std::string_view from, std::string_view to, Date out_date, Date return_date,
                                     int adults);

} // namespace wizz::booking_links
