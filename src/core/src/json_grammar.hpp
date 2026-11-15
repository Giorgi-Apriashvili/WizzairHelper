#pragma once
// Private helpers shared by the JSON reader and writer: the RFC 8259 number
// grammar and a strict UTF-8 decoder (Unicode §3.9, Table 3-7).

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace wizz::json::detail {

enum class NumberProblem : std::uint8_t {
    none,
    missing_integer_digit,   ///< "-" or "-x": no digit after the sign
    leading_zero,            ///< "01", "-00"
    missing_fraction_digit,  ///< "1." or "1.e5"
    missing_exponent_digit,  ///< "1e", "1e+"
};

struct NumberScan {
    std::size_t end = 0;  ///< one past the number, or the offending position when problem != None
    NumberProblem problem = NumberProblem::none;
};

[[nodiscard]] constexpr bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/// Scans `-?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?` at `pos`. `text[pos]` is
/// '-' or a digit. Stops at the first byte that cannot continue the number.
[[nodiscard]] constexpr NumberScan scan_number(std::string_view text, std::size_t pos) noexcept
{
    const auto digit_at = [&](std::size_t i) { return i < text.size() && is_digit(text[i]); };
    const auto skip_digits = [&](std::size_t i) {
        while (digit_at(i)) {
            ++i;
        }
        return i;
    };

    if (text[pos] == '-') {
        ++pos;
    }
    if (!digit_at(pos)) {
        return {pos, NumberProblem::missing_integer_digit};
    }
    if (text[pos] == '0') {
        ++pos;
        if (digit_at(pos)) {
            return {pos, NumberProblem::leading_zero};
        }
    } else {
        pos = skip_digits(pos);
    }
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        if (!digit_at(pos)) {
            return {pos, NumberProblem::missing_fraction_digit};
        }
        pos = skip_digits(pos);
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
            ++pos;
        }
        if (!digit_at(pos)) {
            return {pos, NumberProblem::missing_exponent_digit};
        }
        pos = skip_digits(pos);
    }
    return {pos, NumberProblem::none};
}

/// True when `text` is exactly one JSON number token.
[[nodiscard]] constexpr bool is_number_lexeme(std::string_view text) noexcept
{
    if (text.empty() || (text[0] != '-' && !is_digit(text[0]))) {
        return false;
    }
    const NumberScan scan = scan_number(text, 0);
    return scan.problem == NumberProblem::none && scan.end == text.size();
}

struct DecodedScalar {
    char32_t code_point = 0;  ///< U+FFFD when !valid
    std::size_t length = 0;   ///< bytes consumed: the sequence, or its maximal ill-formed subpart (≥ 1)
    bool valid = false;
};

/// Decodes the UTF-8 sequence at `pos` (< text.size()). Rejects overlong forms,
/// encoded surrogates and values above U+10FFFF.
[[nodiscard]] constexpr DecodedScalar decode_utf8(std::string_view text, std::size_t pos) noexcept
{
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(pos);
    if (lead < 0x80) {
        return {lead, 1, true};
    }

    std::size_t trailing = 0;
    char32_t code_point = 0;
    unsigned char low = 0x80;   // allowed range of the first continuation byte
    unsigned char high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        trailing = 1;
        code_point = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        trailing = 2;
        code_point = lead & 0x0Fu;
        low = lead == 0xE0 ? 0xA0 : 0x80;   // no overlongs
        high = lead == 0xED ? 0x9F : 0xBF;  // no surrogates
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        trailing = 3;
        code_point = lead & 0x07u;
        low = lead == 0xF0 ? 0x90 : 0x80;   // no overlongs
        high = lead == 0xF4 ? 0x8F : 0xBF;  // nothing above U+10FFFF
    } else {
        return {U'�', 1, false};
    }

    std::size_t length = 1;
    for (std::size_t i = 0; i < trailing; ++i, ++length) {
        if (pos + length >= text.size()) {
            return {U'�', length, false};
        }
        const unsigned char next = byte(pos + length);
        if (next < low || next > high) {
            return {U'�', length, false};
        }
        code_point = (code_point << 6) | (next & 0x3Fu);
        low = 0x80;
        high = 0xBF;
    }
    return {code_point, length, true};
}

} // namespace wizz::json::detail
