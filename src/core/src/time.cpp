#include "wizz/core/time.hpp"

#include <charconv>
#include <format>

namespace wizz {

std::string Date::iso() const
{
    return std::format("{:04}-{:02}-{:02}", year(), month(), day());
}

std::optional<Date> Date::parse_iso(std::string_view text)
{
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    const auto number = [&](std::size_t offset, std::size_t length) -> std::optional<int> {
        int value = 0;
        const char* first = text.data() + offset;
        const char* last = first + length;
        for (const char* p = first; p != last; ++p) {
            if (*p < '0' || *p > '9') {
                return std::nullopt;
            }
        }
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc{} || ptr != last) {
            return std::nullopt;
        }
        return value;
    };
    const auto y = number(0, 4);
    const auto m = number(5, 2);
    const auto d = number(8, 2);
    if (!y || !m || !d) {
        return std::nullopt;
    }
    const std::chrono::year_month_day ymd{std::chrono::year{*y}, std::chrono::month{static_cast<unsigned>(*m)},
                                          std::chrono::day{static_cast<unsigned>(*d)}};
    if (!ymd.ok() || *y < 1) {
        return std::nullopt;
    }
    return Date{ymd};
}

} // namespace wizz
