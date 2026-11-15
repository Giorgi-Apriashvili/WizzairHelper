#pragma once
// How grid values compare (01 §8.4, §9.1): numbers and dates by value, strings
// with the locale service at secondary strength (case-insensitive,
// accent-sensitive), empty (null) cells first in ascending order. Equal values
// keep the insertion order: every sort ends with the row's `Idx` ascending, in
// both directions, so the order is total and deterministic.

#include "wizz/core/locale.hpp"

#include <optional>
#include <string_view>

namespace wizz::ui {

enum class SortDirection { ascending, descending };

/// A user header sort: one column, ascending first, then toggled (01 §8.4).
template <class Column>
struct ColumnSort {
    Column column;
    SortDirection direction = SortDirection::ascending;
    friend constexpr bool operator==(const ColumnSort&, const ColumnSort&) = default;
};

/// The header-click rule: the same column toggles its direction, another column
/// starts ascending.
template <class Column>
[[nodiscard]] constexpr ColumnSort<Column> next_sort(const std::optional<ColumnSort<Column>>& current, Column clicked) noexcept
{
    if (current && current->column == clicked) {
        return ColumnSort<Column>{clicked, current->direction == SortDirection::ascending ? SortDirection::descending
                                                                                          : SortDirection::ascending};
    }
    return ColumnSort<Column>{clicked, SortDirection::ascending};
}

/// Three-way comparison by value (<0, 0, >0).
template <class T>
[[nodiscard]] constexpr int compare_values(const T& a, const T& b)
{
    return a < b ? -1 : (b < a ? 1 : 0);
}

/// Null first (DataView treats DBNull as the smallest value), then by value.
template <class T>
[[nodiscard]] constexpr int compare_nullable(const std::optional<T>& a, const std::optional<T>& b)
{
    if (!a) {
        return b ? -1 : 0;
    }
    if (!b) {
        return 1;
    }
    return compare_values(*a, *b);
}

/// Culture string comparison at secondary strength (CurrentCultureIgnoreCase).
[[nodiscard]] inline int compare_text(std::string_view a, std::string_view b, const Locale& locale)
{
    return locale.compare(a, b, CollationStrength::secondary);
}

/// Applies the direction to an ascending comparison result.
[[nodiscard]] constexpr int directed(int ascending_result, SortDirection direction) noexcept
{
    return direction == SortDirection::ascending ? ascending_result : -ascending_result;
}

} // namespace wizz::ui
