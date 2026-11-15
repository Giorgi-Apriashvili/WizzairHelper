#include "wizz/core/text_decoding.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace wizz {
namespace {

using namespace std::string_view_literals;

enum class Encoding { utf8, latin1, utf16le, utf16be, utf32le, utf32be };

constexpr char32_t kReplacement = U'�';

// Byte-order marks (.NET encoding preambles).
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF"sv;
constexpr std::string_view kUtf16LeBom = "\xFF\xFE"sv;
constexpr std::string_view kUtf16BeBom = "\xFE\xFF"sv;
constexpr std::string_view kUtf32LeBom = "\xFF\xFE\0\0"sv;
constexpr std::string_view kUtf32BeBom = "\0\0\xFE\xFF"sv;

struct CharsetName {
    std::string_view name;
    Encoding encoding;
};

// .NET encoding names (EncodingTable) that do not mean UTF-8. Everything else,
// including the UTF-8 aliases and unknown names, decodes as UTF-8 (D-20).
constexpr std::array kCharsetNames{
    CharsetName{"iso-8859-1", Encoding::latin1},     CharsetName{"latin1", Encoding::latin1},
    CharsetName{"l1", Encoding::latin1},             CharsetName{"cp819", Encoding::latin1},
    CharsetName{"csisolatin1", Encoding::latin1},    CharsetName{"ibm819", Encoding::latin1},
    CharsetName{"iso-ir-100", Encoding::latin1},     CharsetName{"iso_8859-1", Encoding::latin1},
    CharsetName{"iso_8859-1:1987", Encoding::latin1}, CharsetName{"iso8859-1", Encoding::latin1},
    CharsetName{"utf-16", Encoding::utf16le},        CharsetName{"utf-16le", Encoding::utf16le},
    CharsetName{"unicode", Encoding::utf16le},       CharsetName{"ucs-2", Encoding::utf16le},
    CharsetName{"iso-10646-ucs-2", Encoding::utf16le}, CharsetName{"utf-16be", Encoding::utf16be},
    CharsetName{"unicodefffe", Encoding::utf16be},   CharsetName{"utf-32", Encoding::utf32le},
    CharsetName{"utf-32le", Encoding::utf32le},      CharsetName{"utf-32be", Encoding::utf32be},
};

[[nodiscard]] constexpr char ascii_lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool iequals_ascii(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

[[nodiscard]] constexpr std::string_view trim_blanks(std::string_view text) noexcept
{
    constexpr std::string_view kBlanks = " \t\r\n";
    const auto first = text.find_first_not_of(kBlanks);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kBlanks) - first + 1);
}

[[nodiscard]] Encoding encoding_for(std::string_view charset) noexcept
{
    const std::string_view name = trim_blanks(charset);
    const auto match = std::ranges::find_if(kCharsetNames, [name](const CharsetName& entry) {
        return iequals_ascii(entry.name, name);
    });
    return match == kCharsetNames.end() ? Encoding::utf8 : match->encoding;
}

[[nodiscard]] constexpr std::string_view preamble(Encoding encoding) noexcept
{
    switch (encoding) {
    case Encoding::utf8: return kUtf8Bom;
    case Encoding::latin1: return {};
    case Encoding::utf16le: return kUtf16LeBom;
    case Encoding::utf16be: return kUtf16BeBom;
    case Encoding::utf32le: return kUtf32LeBom;
    case Encoding::utf32be: return kUtf32BeBom;
    }
    return {};
}

/// StreamReader / HttpContent byte-order-mark detection; UTF-8 when there is none.
[[nodiscard]] Encoding detect_encoding(std::string_view bytes) noexcept
{
    if (bytes.starts_with(kUtf8Bom)) {
        return Encoding::utf8;
    }
    if (bytes.starts_with(kUtf32LeBom)) { // must precede the UTF-16LE check: same first two bytes
        return Encoding::utf32le;
    }
    if (bytes.starts_with(kUtf16LeBom)) {
        return Encoding::utf16le;
    }
    if (bytes.starts_with(kUtf16BeBom)) {
        return Encoding::utf16be;
    }
    if (bytes.starts_with(kUtf32BeBom)) {
        return Encoding::utf32be;
    }
    return Encoding::utf8;
}

[[nodiscard]] constexpr std::uint8_t byte_at(std::string_view bytes, std::size_t index) noexcept
{
    return static_cast<std::uint8_t>(bytes[index]);
}

void append_utf8(std::string& out, char32_t code_point)
{
    const auto put = [&out](std::uint32_t value) { out.push_back(static_cast<char>(value)); };
    const auto cp = static_cast<std::uint32_t>(code_point);
    if (cp < 0x80) {
        put(cp);
    } else if (cp < 0x800) {
        put(0xC0 | (cp >> 6));
        put(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        put(0xE0 | (cp >> 12));
        put(0x80 | ((cp >> 6) & 0x3F));
        put(0x80 | (cp & 0x3F));
    } else {
        put(0xF0 | (cp >> 18));
        put(0x80 | ((cp >> 12) & 0x3F));
        put(0x80 | ((cp >> 6) & 0x3F));
        put(0x80 | (cp & 0x3F));
    }
}

[[nodiscard]] constexpr bool is_surrogate(std::uint32_t value) noexcept
{
    return value >= 0xD800 && value <= 0xDFFF;
}

/// Copies well-formed UTF-8 (Unicode Table 3-7) and replaces each maximal
/// ill-formed subpart with U+FFFD, like the .NET Core 3.0+ UTF-8 decoder.
[[nodiscard]] std::string decode_utf8(std::string_view bytes)
{
    std::string out;
    out.reserve(bytes.size());
    std::size_t i = 0;
    while (i < bytes.size()) {
        const std::uint8_t lead = byte_at(bytes, i);
        if (lead < 0x80) {
            out.push_back(bytes[i]);
            ++i;
            continue;
        }
        std::size_t length = 0;
        std::uint8_t second_low = 0x80;
        std::uint8_t second_high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            second_low = lead == 0xE0 ? std::uint8_t{0xA0} : std::uint8_t{0x80};
            second_high = lead == 0xED ? std::uint8_t{0x9F} : std::uint8_t{0xBF};
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            second_low = lead == 0xF0 ? std::uint8_t{0x90} : std::uint8_t{0x80};
            second_high = lead == 0xF4 ? std::uint8_t{0x8F} : std::uint8_t{0xBF};
        } else {
            append_utf8(out, kReplacement);
            ++i;
            continue;
        }
        std::size_t valid = 1; // bytes of the sequence that are well-formed so far
        while (valid < length && i + valid < bytes.size()) {
            const std::uint8_t next = byte_at(bytes, i + valid);
            const std::uint8_t low = valid == 1 ? second_low : std::uint8_t{0x80};
            const std::uint8_t high = valid == 1 ? second_high : std::uint8_t{0xBF};
            if (next < low || next > high) {
                break;
            }
            ++valid;
        }
        if (valid == length) {
            out.append(bytes.substr(i, length));
        } else {
            append_utf8(out, kReplacement);
        }
        i += valid;
    }
    return out;
}

[[nodiscard]] std::string decode_latin1(std::string_view bytes)
{
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        append_utf8(out, byte_at(bytes, i));
    }
    return out;
}

[[nodiscard]] std::string decode_utf16(std::string_view bytes, bool big_endian)
{
    const auto unit_at = [&](std::size_t unit) -> std::uint32_t {
        const std::uint32_t first = byte_at(bytes, 2 * unit);
        const std::uint32_t second = byte_at(bytes, 2 * unit + 1);
        return big_endian ? (first << 8) | second : (second << 8) | first;
    };
    const std::size_t units = bytes.size() / 2;
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t u = 0; u < units; ++u) {
        const std::uint32_t unit = unit_at(u);
        if (!is_surrogate(unit)) {
            append_utf8(out, unit);
            continue;
        }
        if (unit <= 0xDBFF && u + 1 < units) {
            const std::uint32_t low = unit_at(u + 1);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                append_utf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                ++u;
                continue;
            }
        }
        append_utf8(out, kReplacement); // unpaired surrogate
    }
    if (bytes.size() % 2 != 0) {
        append_utf8(out, kReplacement);
    }
    return out;
}

[[nodiscard]] std::string decode_utf32(std::string_view bytes, bool big_endian)
{
    std::string out;
    out.reserve(bytes.size());
    const std::size_t units = bytes.size() / 4;
    for (std::size_t u = 0; u < units; ++u) {
        std::uint32_t value = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const std::uint32_t b = byte_at(bytes, 4 * u + k);
            value |= big_endian ? b << (8 * (3 - k)) : b << (8 * k);
        }
        append_utf8(out, value > 0x10FFFF || is_surrogate(value) ? kReplacement : value);
    }
    if (bytes.size() % 4 != 0) {
        append_utf8(out, kReplacement);
    }
    return out;
}

} // namespace

std::string decode_text(std::string_view bytes, std::optional<std::string_view> charset)
{
    const Encoding encoding = charset ? encoding_for(*charset) : detect_encoding(bytes);
    if (const std::string_view bom = preamble(encoding); !bom.empty() && bytes.starts_with(bom)) {
        bytes.remove_prefix(bom.size());
    }
    switch (encoding) {
    case Encoding::utf8: return decode_utf8(bytes);
    case Encoding::latin1: return decode_latin1(bytes);
    case Encoding::utf16le: return decode_utf16(bytes, false);
    case Encoding::utf16be: return decode_utf16(bytes, true);
    case Encoding::utf32le: return decode_utf32(bytes, false);
    case Encoding::utf32be: return decode_utf32(bytes, true);
    }
    return decode_utf8(bytes);
}

} // namespace wizz
