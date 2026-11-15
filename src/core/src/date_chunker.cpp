#include "wizz/core/date_chunker.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace wizz::date_chunker {

std::vector<std::pair<Date, Date>> split(Date from, Date to, int max_days)
{
    if (max_days < 1) {
        throw std::invalid_argument("max_days must be at least 1.");
    }
    std::vector<std::pair<Date, Date>> chunks;
    for (Date start = from; start <= to;) {
        // Offsets stay within [0, to − start], so no overflow for any max_days.
        const std::int64_t span = std::min<std::int64_t>(std::int64_t{max_days} - 1, days_between(to, start));
        const Date end = start.add_days(span);
        chunks.emplace_back(start, end);
        start = end.add_days(1);
    }
    return chunks;
}

} // namespace wizz::date_chunker
