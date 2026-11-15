#pragma once
// Private helpers shared by planner.cpp and planner_rules.cpp (03 §4).

#include "wizz/core/format.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/text.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wizz::planner::detail {

/// `set_ci` of 03 §4.2: codes compared ordinally ignoring case.
using CodeSet = std::unordered_set<std::string, text::OrdinalIgnoreCaseHash, text::OrdinalIgnoreCaseEqual>;

/// `insertion_ordered_map_ci` of 03 §4.2: keys compared ordinally ignoring
/// case, enumerated in first-insertion order; assigning to an existing key
/// keeps its position (and its first spelling).
template <class Value>
class OrderedCodeMap {
public:
    using Entry = std::pair<std::string, Value>;

    [[nodiscard]] Value* find(std::string_view key)
    {
        const auto it = index_.find(key);
        return it == index_.end() ? nullptr : &entries_[it->second].second;
    }

    /// The value of `key`, inserting a default-constructed one at the end if absent.
    [[nodiscard]] Value& operator[](std::string_view key)
    {
        if (Value* existing = find(key)) {
            return *existing;
        }
        index_.emplace(std::string{key}, entries_.size());
        return entries_.emplace_back(std::string{key}, Value{}).second;
    }

    [[nodiscard]] std::span<const Entry> entries() const noexcept { return entries_; }
    [[nodiscard]] std::span<Entry> entries() noexcept { return entries_; }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t, text::OrdinalIgnoreCaseHash, text::OrdinalIgnoreCaseEqual> index_;
};

/// `{x:0}` of 03 §0 for a distance or radius label (07 §2).
[[nodiscard]] inline std::string format_km(double value, const Locale& locale)
{
    return format_label(value, 0, locale.number_symbols());
}

/// The first `limit` items of `items` joined with `separator` (`join(sep, first n of …)`).
[[nodiscard]] inline std::string join(std::span<const std::string> items, std::string_view separator,
                                      std::size_t limit = static_cast<std::size_t>(-1))
{
    std::string result;
    const std::size_t count = items.size() < limit ? items.size() : limit;
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            result += separator;
        }
        result += items[i];
    }
    return result;
}

} // namespace wizz::planner::detail
