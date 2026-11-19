// CityNames (DECISIONS D-72).

#include "wizz/core/city_names.hpp"

#include "wizz/core/text.hpp"

#include <algorithm>
#include <string_view>

namespace wizz::city_names {

namespace {

constexpr char16_t kEnDash = u'–';
constexpr char16_t kEmDash = u'—';

/// White-space runs (line breaks included) as one space, none at either end.
[[nodiscard]] std::u16string collapse_white_space(std::u16string_view text)
{
    std::u16string out;
    bool space_pending = false;
    for (const char16_t unit : text) {
        if (text::is_white_space(unit)) {
            space_pending = !out.empty();
            continue;
        }
        if (space_pending) {
            out += u' ';
            space_pending = false;
        }
        out += unit;
    }
    return out;
}

/// `text` without its "(…)" parts; an unclosed "(" drops the rest.
[[nodiscard]] std::u16string without_brackets(std::u16string_view text)
{
    std::u16string out;
    int depth = 0;
    for (const char16_t unit : text) {
        if (unit == u'(') {
            ++depth;
        } else if (unit == u')' && depth > 0) {
            --depth;
        } else if (depth == 0) {
            out += unit;
        }
    }
    return out;
}

/// `text` (single spaces) up to a "/" or a dash with a space on both sides.
[[nodiscard]] std::u16string_view before_separator(std::u16string_view text)
{
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char16_t unit = text[i];
        if (unit == u'/') {
            return text.substr(0, i);
        }
        const bool dash = unit == u'-' || unit == kEnDash || unit == kEmDash;
        if (dash && i > 0 && i + 1 < text.size() && text[i - 1] == u' ' && text[i + 1] == u' ') {
            return text.substr(0, i);
        }
    }
    return text;
}

[[nodiscard]] std::u16string clean_name16(std::string_view station_name)
{
    const std::u16string collapsed = collapse_white_space(without_brackets(text::utf8_to_utf16(station_name)));
    return collapse_white_space(before_separator(collapsed));
}

/// text::normalize_key with hyphens read as spaces.
[[nodiscard]] std::u16string city_key(std::string_view name)
{
    std::u16string units = text::utf8_to_utf16(name);
    std::ranges::replace(units, u'-', u' ');
    return text::normalize_key(units);
}

} // namespace

std::string clean_name(std::string_view station_name)
{
    return text::utf16_to_utf8(clean_name16(station_name));
}

std::vector<std::string> candidates(std::string_view station_name)
{
    const std::u16string name = clean_name16(station_name);
    std::vector<std::size_t> word_ends;  // single spaces only, after clean_name16
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == u' ') {
            word_ends.push_back(i);
        }
    }
    if (!name.empty()) {
        word_ends.push_back(name.size());
    }

    std::vector<std::string> out;
    const auto add = [&out](std::u16string_view candidate) {
        if (candidate.size() < kMinLength) {
            return;
        }
        std::string utf8 = text::utf16_to_utf8(candidate);
        if (std::ranges::find(out, utf8) == out.end()) {
            out.push_back(std::move(utf8));
        }
    };
    for (std::size_t words = std::min(word_ends.size(), kMaxWords); words >= 1; --words) {
        add(std::u16string_view{name}.substr(0, word_ends[words - 1]));
    }
    if (!word_ends.empty()) {
        const std::u16string_view first_word = std::u16string_view{name}.substr(0, word_ends.front());
        if (const std::size_t hyphen = first_word.find(u'-'); hyphen != std::u16string_view::npos) {
            add(first_word.substr(0, hyphen));
        }
    }
    return out;
}

bool names_city(std::string_view hit_name, std::string_view candidate)
{
    const std::u16string wanted = city_key(candidate);
    const std::u16string hit = city_key(hit_name);
    return !wanted.empty() && hit.starts_with(wanted) &&
           (hit.size() == wanted.size() || !text::is_letter_or_digit(hit[wanted.size()]));
}

} // namespace wizz::city_names
