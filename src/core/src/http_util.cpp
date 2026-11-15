// Pure helpers declared in wizz/core/http.hpp (04 §2.2, 02 §6.5).

#include "wizz/core/http.hpp"
#include "wizz/core/text_decoding.hpp"

#include <algorithm>
#include <cstdint>

namespace wizz::http {
namespace {

[[nodiscard]] constexpr std::string_view trim_http_whitespace(std::string_view text) noexcept
{
    constexpr std::string_view kWhitespace = " \t\r\n";
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kWhitespace) - first + 1);
}

[[nodiscard]] constexpr bool iequals_ascii(std::string_view a, std::string_view b) noexcept
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() && std::ranges::equal(a, b, [&](char x, char y) { return lower(x) == lower(y); });
}

/// The first `charset` parameter of a Content-Type value with at most one pair of
/// surrounding quotes removed (HttpContent.GetEncodingFromCharset); nullopt when absent.
[[nodiscard]] std::optional<std::string_view> charset_parameter(std::string_view content_type) noexcept
{
    std::size_t separator = content_type.find(';');
    while (separator != std::string_view::npos) {
        const std::size_t next = content_type.find(';', separator + 1);
        const std::string_view parameter = content_type.substr(
            separator + 1, next == std::string_view::npos ? std::string_view::npos : next - separator - 1);
        if (const auto equals = parameter.find('=');
            equals != std::string_view::npos && iequals_ascii(trim_http_whitespace(parameter.substr(0, equals)), "charset")) {
            std::string_view value = trim_http_whitespace(parameter.substr(equals + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
        separator = next;
    }
    return std::nullopt;
}

/// RFC 3986 unreserved characters, the only bytes Uri.EscapeDataString keeps.
[[nodiscard]] constexpr bool is_unreserved(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~';
}

} // namespace

std::string media_type(const Response& response)
{
    if (!response.content_type) {
        return {};
    }
    const std::string_view value = *response.content_type;
    return std::string{trim_http_whitespace(value.substr(0, value.find(';')))};
}

std::string decode_body_text(const Response& response)
{
    const std::optional<std::string_view> charset =
        response.content_type ? charset_parameter(*response.content_type) : std::nullopt;
    return decode_text(response.body, charset);
}

std::string escape_data_string(std::string_view text)
{
    constexpr std::string_view kHexDigits = "0123456789ABCDEF";
    std::string escaped;
    escaped.reserve(text.size() * 3);
    for (const char c : text) {
        if (is_unreserved(c)) {
            escaped.push_back(c);
            continue;
        }
        const auto byte = static_cast<std::uint8_t>(c);
        escaped.push_back('%');
        escaped.push_back(kHexDigits[byte >> 4]);
        escaped.push_back(kHexDigits[byte & 0x0F]);
    }
    return escaped;
}

} // namespace wizz::http
