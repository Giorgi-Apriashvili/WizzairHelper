// BookingLinks (03 §9).

#include "wizz/core/booking_links.hpp"

#include "wizz/core/text.hpp"

#include <algorithm>
#include <format>

namespace wizz::booking_links {

namespace {

/// `{Base}/{FROM}/{TO}/{outbound}/{return_segment}/{adults}/0/0/null`.
[[nodiscard]] std::string link(std::string_view from, std::string_view to, Date out_date,
                               std::string_view return_segment, int adults)
{
    return std::format("{}/{}/{}/{}/{}/{}/0/0/null", kBase, text::to_upper_invariant(from),
                       text::to_upper_invariant(to), out_date.iso(), return_segment, std::max(1, adults));
}

} // namespace

std::string one_way(std::string_view from, std::string_view to, Date date, int adults)
{
    return link(from, to, date, "null", adults);
}

std::string round_trip(std::string_view from, std::string_view to, Date out_date, Date return_date, int adults)
{
    return link(from, to, out_date, return_date.iso(), adults);
}

} // namespace wizz::booking_links
