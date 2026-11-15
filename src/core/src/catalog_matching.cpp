// Offline place matching of the AirportCatalog (02 §5.4–§5.8): pseudo-station
// city names, MatchPlace R1..R7, PlaceFor / CityPlace, whole-word prefixes and
// typo correction against Wizz names.

#include "wizz/core/catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <set>
#include <utility>

namespace wizz {

namespace {

/// " — Wizz Air airports: " with U+2014 EM DASH and ASCII spaces (02 §5.6).
constexpr std::string_view kAirportsListLabel = " \u2014 Wizz Air airports: ";

/// Separators of the NameIndex first word (02 §5.8: `Split(' ', '/', '(', '-')`).
constexpr std::string_view kFirstWordSeparators = " /(-";
constexpr std::u16string_view kFirstWordSeparators16 = u" /(-";

constexpr std::size_t kNoMatch = std::u16string_view::npos;

[[nodiscard]] bool is_space(char16_t c) noexcept
{
    return text::is_white_space(c);  // .NET regex \s == char.IsWhiteSpace
}

[[nodiscard]] bool is_ascii_letter(char16_t c) noexcept
{
    return (c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z');
}

/// .NET RegexCharClass.IsBoundaryWordChar: L*, Mn, Nd, Pc, ZWNJ, ZWJ. A lone
/// surrogate unit (half of a supplementary character) is not a word character.
[[nodiscard]] bool is_boundary_word_char(char16_t c) noexcept
{
    if (c == u'\u200C' || c == u'\u200D') {
        return true;
    }
    using enum text::UnicodeCategory;
    switch (text::general_category(c)) {
    case uppercase_letter:
    case lowercase_letter:
    case titlecase_letter:
    case modifier_letter:
    case other_letter:
    case nonspacing_mark:
    case decimal_digit_number:
    case connector_punctuation:
        return true;
    default:
        return false;
    }
}

/// RegexOptions.IgnoreCase equality of a text unit with a lower-case ASCII
/// pattern character: ASCII case folding, plus U+0130 for 'i' (the en-US / invariant
/// case-equivalence table of .NET 7+).
[[nodiscard]] bool equals_ignore_case(char16_t c, char16_t pattern) noexcept
{
    const char16_t lower = c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c + (u'a' - u'A')) : c;
    return lower == pattern || (pattern == u'i' && c == u'\u0130');
}

[[nodiscard]] bool equals_ignore_case(std::u16string_view s, std::u16string_view pattern) noexcept
{
    return s.size() == pattern.size() &&
           std::ranges::equal(s, pattern, [](char16_t c, char16_t p) { return equals_ignore_case(c, p); });
}

[[nodiscard]] std::size_t end_without_trailing_space(std::u16string_view s) noexcept
{
    std::size_t end = s.size();
    while (end > 0 && is_space(s[end - 1])) {
        --end;
    }
    return end;
}

/// Index where `\s*(\(\s*all airports\s*\)|\bany)\s*$` (IgnoreCase) starts its
/// token, or kNoMatch. The leading `\s*` of the pattern only widens the removed
/// span over white space that the final Trim removes anyway.
[[nodiscard]] std::size_t pseudo_suffix_start(std::u16string_view s) noexcept
{
    const std::size_t end = end_without_trailing_space(s);

    constexpr std::u16string_view kAny = u"any";
    if (end >= kAny.size() && equals_ignore_case(s.substr(end - kAny.size(), kAny.size()), kAny)) {
        const std::size_t start = end - kAny.size();
        // \b before 'a' (a word character): the previous unit must not be one.
        return start == 0 || !is_boundary_word_char(s[start - 1]) ? start : kNoMatch;
    }

    if (end == 0 || s[end - 1] != u')') {
        return kNoMatch;
    }
    std::size_t i = end - 1;
    while (i > 0 && is_space(s[i - 1])) {
        --i;
    }
    constexpr std::u16string_view kAllAirports = u"all airports";  // exactly one literal space
    if (i < kAllAirports.size() || !equals_ignore_case(s.substr(i - kAllAirports.size(), kAllAirports.size()), kAllAirports)) {
        return kNoMatch;
    }
    i -= kAllAirports.size();
    while (i > 0 && is_space(s[i - 1])) {
        --i;
    }
    return i > 0 && s[i - 1] == u'(' ? i - 1 : kNoMatch;
}

/// R1 TrailingCodeRegex `\(\s*([A-Za-z]{3})\s*\)\s*$`: the three letters, or nullopt.
[[nodiscard]] std::optional<std::u16string_view> trailing_code(std::u16string_view s) noexcept
{
    const std::size_t end = end_without_trailing_space(s);
    if (end == 0 || s[end - 1] != u')') {
        return std::nullopt;
    }
    std::size_t i = end - 1;
    while (i > 0 && is_space(s[i - 1])) {
        --i;
    }
    constexpr std::size_t kCodeLength = 3;
    if (i < kCodeLength || !std::ranges::all_of(s.substr(i - kCodeLength, kCodeLength), is_ascii_letter)) {
        return std::nullopt;
    }
    const std::size_t code_start = i - kCodeLength;
    std::size_t j = code_start;
    while (j > 0 && is_space(s[j - 1])) {
        --j;
    }
    if (j == 0 || s[j - 1] != u'(') {
        return std::nullopt;
    }
    return s.substr(code_start, kCodeLength);
}

/// `codes joined ", "`.
[[nodiscard]] std::string join_codes(const std::vector<std::string>& codes)
{
    std::string joined;
    for (const std::string& code : codes) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += code;
    }
    return joined;
}

[[nodiscard]] std::string_view first_word(std::string_view s) noexcept
{
    return s.substr(0, s.find_first_of(kFirstWordSeparators));
}

[[nodiscard]] std::u16string_view first_word(std::u16string_view s) noexcept
{
    return s.substr(0, s.find_first_of(kFirstWordSeparators16));
}

[[nodiscard]] std::u16string name_key(std::string_view name)
{
    return text::normalize_key(text::utf8_to_utf16(name));
}

/// `macs = distinct(a.Mac)` with `|macs| == 1 and macs[0] != null` (02 §5.5 R6, §5.6
/// CityPlace; null counts as a value, comparison is ordinal): that shared mac, else nullptr.
[[nodiscard]] const std::string* shared_mac(std::span<const AirportPtr> airports) noexcept
{
    if (airports.empty() || !airports.front()->mac) {
        return nullptr;
    }
    const std::optional<std::string>& first = airports.front()->mac;
    const bool shared = std::ranges::all_of(airports, [&first](const AirportPtr& a) { return a->mac == first; });
    return shared ? &*first : nullptr;
}

} // namespace

std::string AirportCatalog::city_name_of_fake(std::string_view name)
{
    const std::u16string utf16 = text::utf8_to_utf16(name);
    const std::size_t cut = pseudo_suffix_start(utf16);
    if (cut == kNoMatch) {
        return std::string{text::trim(name)};
    }
    return text::utf16_to_utf8(text::trim(std::u16string_view{utf16}.substr(0, cut)));
}

std::optional<Place> AirportCatalog::match_place(std::string_view input) const
{
    if (text::is_null_or_white_space(input)) {
        return std::nullopt;
    }
    const std::string_view trimmed = text::trim(input);
    const std::u16string trimmed16 = text::utf8_to_utf16(trimmed);

    // R1: "Something (XXX)" naming a known station keeps the whole text as the name.
    if (const std::optional<std::u16string_view> code = trailing_code(trimmed16)) {
        if (const AirportPtr airport = find(text::utf16_to_utf8(*code))) {
            return place_for(*airport, trimmed);
        }
    }

    const std::u16string key = text::normalize_key(trimmed16);
    if (key.empty()) {
        return std::nullopt;
    }

    // R2: exactly one real station carries this name.
    std::vector<AirportPtr> exact_real;
    for (std::size_t i = 0; i < real_.size(); ++i) {
        if (real_keys_[i] == key) {
            exact_real.push_back(real_[i]);
        }
    }
    if (exact_real.size() == 1) {
        return place_for(*exact_real.front(), exact_real.front()->name);
    }

    // R3: the first pseudo station (map order) whose city name matches.
    for (const FakeCity& fake : fake_cities_) {
        if (fake.key == key) {
            return place_for(*fake.airport, fake.city_name);
        }
    }

    // R4: an IATA code typed directly (any case, pseudo codes too).
    if (key.size() == 3) {
        if (const AirportPtr airport = find(text::utf16_to_utf8(key))) {
            return place_for(*airport, airport->name);
        }
    }

    // R5: several real stations share the exact name.
    if (exact_real.size() > 1) {
        return city_place(exact_real, trimmed);
    }

    // R6: whole-word prefixes trusted only inside one group that has a pseudo station.
    const std::vector<AirportPtr> prefixed = prefix_matches(trimmed);
    if (!prefixed.empty()) {
        if (const std::string* mac = shared_mac(prefixed); mac != nullptr && fake_with_iata(*mac) != nullptr) {
            return city_place(prefixed, trimmed);
        }
    }

    return std::nullopt;  // R7
}

Place AirportCatalog::place_for(const Airport& airport, std::string_view display_name) const
{
    if (airport.is_fake_station) {
        std::vector<std::string> codes;
        for (const AirportPtr& member : group_members(airport.iata)) {
            codes.push_back(member->iata);
        }
        std::string description = std::string{display_name} + ", " + airport.country_name +
                                  std::string{kAirportsListLabel} + join_codes(codes);
        return Place{std::string{display_name}, std::move(description), airport.location, PlaceSource::wizz_city,
                     std::move(codes)};
    }
    return Place{std::string{display_name}, airport.name + " (" + airport.iata + "), " + airport.country_name,
                 airport.location, PlaceSource::wizz_airport, std::vector<std::string>{airport.iata}};
}

Place AirportCatalog::city_place(std::span<const AirportPtr> airports, std::string_view typed) const
{
    const std::string* mac = shared_mac(airports);
    const Airport* fake = mac != nullptr ? fake_with_iata(*mac) : nullptr;

    GeoPoint location;
    if (fake != nullptr) {
        location = fake->location;
    } else {
        std::vector<GeoPoint> points;
        points.reserve(airports.size());
        for (const AirportPtr& airport : airports) {
            points.push_back(airport->location);
        }
        location = centroid(points);  // std::invalid_argument("No points given.") when empty
    }

    std::string name = locale_->title_case(text::to_lower_invariant(typed));
    std::vector<std::string> codes;
    codes.reserve(airports.size());
    for (const AirportPtr& airport : airports) {
        codes.push_back(airport->iata);
    }
    std::string description =
        name + ", " + airports.front()->country_name + std::string{kAirportsListLabel} + join_codes(codes);
    return Place{std::move(name), std::move(description), location, PlaceSource::wizz_city, std::move(codes)};
}

std::vector<AirportPtr> AirportCatalog::prefix_matches(std::string_view input) const
{
    const std::u16string key = name_key(input);
    std::vector<AirportPtr> matches;
    if (key.size() < 3) {
        return matches;
    }
    for (std::size_t i = 0; i < real_.size(); ++i) {
        const std::u16string& name = real_keys_[i];
        if (name.size() > key.size() && name.starts_with(key) && !text::is_letter_or_digit(name[key.size()])) {
            matches.push_back(real_[i]);
        }
    }
    return matches;
}

std::optional<Place> AirportCatalog::prefix_place(std::string_view input) const
{
    const std::vector<AirportPtr> matches = prefix_matches(input);
    if (matches.empty()) {
        return std::nullopt;
    }
    return city_place(matches, text::trim(input));
}

std::optional<std::string> AirportCatalog::fuzzy_correct(std::string_view input) const
{
    const std::u16string key = name_key(input);
    if (key.size() < 4) {
        return std::nullopt;
    }
    const std::size_t max_distance = key.size() <= 7 ? 1 : 2;

    const std::string* best = nullptr;
    int best_distance = std::numeric_limits<int>::max();
    bool ambiguous = false;
    for (const NameIndexEntry& entry : name_index_) {
        const std::size_t length_difference =
            entry.key.size() > key.size() ? entry.key.size() - key.size() : key.size() - entry.key.size();
        if (length_difference > max_distance) {
            continue;
        }
        const int distance = edit_distance(key, entry.key);
        if (distance == 0 || static_cast<std::size_t>(distance) > max_distance) {
            continue;  // an exact name is never a "correction"
        }
        if (distance < best_distance) {
            best = &entry.display;
            best_distance = distance;
            ambiguous = false;
        } else if (distance == best_distance && !text::ordinal_ignore_case_equals(*best, entry.display)) {
            ambiguous = true;
        }
    }
    if (ambiguous || best == nullptr) {
        return std::nullopt;
    }
    return *best;
}

std::vector<NameIndexEntry> AirportCatalog::build_name_index(std::span<const AirportPtr> all)
{
    std::vector<NameIndexEntry> index;
    std::set<std::pair<std::u16string, std::string>> seen;  // ordinal (key, display) pairs
    const auto add = [&index, &seen](std::u16string key, std::string display) {
        if (seen.emplace(key, display).second) {
            index.push_back(NameIndexEntry{std::move(key), std::move(display)});
        }
    };
    for (const AirportPtr& airport : all) {
        std::string name = airport->is_fake_station ? city_name_of_fake(airport->name) : airport->name;
        std::u16string key = name_key(name);
        const std::u16string_view word_key = first_word(key);
        std::optional<NameIndexEntry> word;
        if (word_key.size() >= 4) {
            word = NameIndexEntry{std::u16string{word_key}, std::string{first_word(name)}};
        }
        add(std::move(key), std::move(name));
        if (word) {
            add(std::move(word->key), std::move(word->display));
        }
    }
    return index;
}

int AirportCatalog::edit_distance(std::u16string_view a, std::u16string_view b)
{
    // Three rolling rows of the OSA matrix: d[i-2], d[i-1], d[i].
    const std::size_t columns = b.size() + 1;
    std::vector<int> before_previous(columns);
    std::vector<int> previous(columns);
    std::vector<int> current(columns);
    for (std::size_t j = 0; j < columns; ++j) {
        previous[j] = static_cast<int>(j);
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        current[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            int value = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                value = std::min(value, before_previous[j - 2] + 1);
            }
            current[j] = value;
        }
        std::swap(before_previous, previous);
        std::swap(previous, current);
    }
    return previous[b.size()];
}

} // namespace wizz
