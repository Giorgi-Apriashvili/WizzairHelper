#pragma once
// DateChunker.Split (03 §2, 04 §9.1): inclusive date windows of at most
// `max_days` days, used for the 42-day Wizz timetable limit.

#include "wizz/core/time.hpp"

#include <utility>
#include <vector>

namespace wizz::date_chunker {

/// Splits the inclusive range [from, to] into consecutive inclusive chunks:
/// `start = from; while start <= to: end = min(start + max_days − 1, to); emit (start, end); start = end + 1`.
/// Returns an empty list when `to < from`. Throws std::invalid_argument when `max_days < 1`.
[[nodiscard]] std::vector<std::pair<Date, Date>> split(Date from, Date to, int max_days);

} // namespace wizz::date_chunker
