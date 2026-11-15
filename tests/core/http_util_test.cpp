// media_type, decode_body_text and escape_data_string (04 §2.2, 02 §6.5).

#include "wizz/core/http.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {

using namespace std::string_literals;
using wizz::http::Response;

Response response_with(std::optional<std::string> content_type, std::string body = {})
{
    Response response;
    response.status = 200;
    response.content_type = std::move(content_type);
    response.body = std::move(body);
    return response;
}

// --- media_type (04 §2.2 "MediaType": the part before ';', trimmed; missing → "") ---

TEST(HttpMediaType, Missing_content_type_is_empty)
{
    EXPECT_EQ(wizz::http::media_type(response_with(std::nullopt)), "");
}

TEST(HttpMediaType, Parameters_are_dropped)
{
    EXPECT_EQ(wizz::http::media_type(response_with("application/json; charset=utf-8")), "application/json");
    EXPECT_EQ(wizz::http::media_type(response_with("text/html;charset=utf-8")), "text/html");
    EXPECT_EQ(wizz::http::media_type(response_with("text/plain")), "text/plain");
}

TEST(HttpMediaType, Value_is_trimmed_and_keeps_its_case)
{
    EXPECT_EQ(wizz::http::media_type(response_with("  Application/JSON \t; charset=utf-8")), "Application/JSON");
    EXPECT_EQ(wizz::http::media_type(response_with("")), "");
    EXPECT_EQ(wizz::http::media_type(response_with(" ; charset=utf-8")), "");
}

// --- decode_body_text (04 §2.2 "Body decoding", D-20) ---

TEST(HttpDecodeBodyText, Utf8_charset_decodes_and_strips_the_bom)
{
    EXPECT_EQ(wizz::http::decode_body_text(response_with("application/json; charset=utf-8", "\xEF\xBB\xBF{\"a\":\"\xC3\xA9\"}")),
              "{\"a\":\"\xC3\xA9\"}");
}

TEST(HttpDecodeBodyText, Charset_name_is_case_insensitive_and_may_be_quoted)
{
    EXPECT_EQ(wizz::http::decode_body_text(response_with("text/html; Charset=\"ISO-8859-1\"", "caf\xE9")), "caf\xC3\xA9");
    EXPECT_EQ(wizz::http::decode_body_text(response_with("text/html; foo=bar; CHARSET = latin1 ", "\xE9")), "\xC3\xA9");
}

TEST(HttpDecodeBodyText, Iso_8859_1_maps_every_byte_to_its_code_point)
{
    // Verified .NET behaviour: charset=iso-8859-1 decodes as Latin-1.
    EXPECT_EQ(wizz::http::decode_body_text(response_with("text/html; charset=iso-8859-1", "\xA3\xFF\x41")),
              "\xC2\xA3\xC3\xBF" "A");
    // Latin-1 has no byte-order mark, so UTF-8 BOM bytes are ordinary characters.
    EXPECT_EQ(wizz::http::decode_body_text(response_with("text/plain; charset=iso-8859-1", "\xEF\xBB\xBFx")),
              "\xC3\xAF\xC2\xBB\xC2\xBFx");
}

TEST(HttpDecodeBodyText, Unknown_charset_decodes_as_utf8_D20)
{
    EXPECT_EQ(wizz::http::decode_body_text(response_with("text/plain; charset=klingon", "Kraków")), "Kraków");
    EXPECT_EQ(wizz::http::decode_body_text(response_with("text/plain; charset=windows-1252", "\xEF\xBB\xBFok")), "ok");
    EXPECT_EQ(wizz::http::decode_body_text(response_with("text/plain; charset=klingon", "a\xFF")), "a\xEF\xBF\xBD");
}

TEST(HttpDecodeBodyText, Without_charset_a_bom_selects_the_encoding_and_is_stripped)
{
    EXPECT_EQ(wizz::http::decode_body_text(response_with("application/json", "\xEF\xBB\xBF[1]")), "[1]");
    EXPECT_EQ(wizz::http::decode_body_text(response_with(std::nullopt, "\xFF\xFE" "h\0i\0"s)), "hi");
    EXPECT_EQ(wizz::http::decode_body_text(response_with(std::nullopt, "\xFE\xFF\0h\0i"s)), "hi");
    EXPECT_EQ(wizz::http::decode_body_text(response_with(std::nullopt, "\xFF\xFE\0\0h\0\0\0"s)), "h");
    EXPECT_EQ(wizz::http::decode_body_text(response_with(std::nullopt, "\0\0\xFE\xFF\0\0\0h"s)), "h");
}

TEST(HttpDecodeBodyText, Without_charset_or_bom_the_body_is_utf8)
{
    EXPECT_EQ(wizz::http::decode_body_text(response_with("text/html", "Krak\xC3\xB3w")), "Krak\xC3\xB3w");
    EXPECT_EQ(wizz::http::decode_body_text(response_with(std::nullopt, "")), "");
}

TEST(HttpDecodeBodyText, Html_meta_charset_is_never_sniffed)
{
    // stale_version_404.html declares iso-8859-1 in a <meta> tag; only the header counts (04 §2.2).
    const std::string html = "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=iso-8859-1\"/>\xE9";
    EXPECT_EQ(wizz::http::decode_body_text(response_with("text/html", html)),
              "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=iso-8859-1\"/>\xEF\xBF\xBD");
}

TEST(HttpDecodeBodyText, Invalid_utf8_becomes_replacement_characters)
{
    EXPECT_EQ(wizz::http::decode_body_text(response_with("application/json; charset=utf-8", "{\"x\":\"\xC3\x28\"}")),
              "{\"x\":\"\xEF\xBF\xBD(\"}");
}

TEST(HttpDecodeBodyText, Explicit_charset_strips_only_its_own_bom)
{
    // A UTF-16 BOM under charset=utf-8 is two ill-formed UTF-8 bytes, not a BOM.
    EXPECT_EQ(wizz::http::decode_body_text(response_with("text/plain; charset=utf-8", "\xFF\xFEok")),
              "\xEF\xBF\xBD\xEF\xBF\xBDok");
    EXPECT_EQ(wizz::http::decode_body_text(response_with("text/plain; charset=utf-16", "\xFF\xFE" "o\0k\0"s)), "ok");
}

// --- escape_data_string (02 §6.5: Uri.EscapeDataString, RFC 3986, upper-case hex) ---

TEST(HttpEscapeDataString, Krakow_is_percent_encoded_as_utf8)
{
    EXPECT_EQ(wizz::http::escape_data_string("Kraków"), "Krak%C3%B3w");
}

TEST(HttpEscapeDataString, Space_becomes_percent_20_never_plus)
{
    EXPECT_EQ(wizz::http::escape_data_string("New York"), "New%20York");
    EXPECT_EQ(wizz::http::escape_data_string("a+b"), "a%2Bb");
}

TEST(HttpEscapeDataString, Unreserved_characters_are_kept)
{
    const std::string unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    EXPECT_EQ(wizz::http::escape_data_string(unreserved), unreserved);
    EXPECT_EQ(wizz::http::escape_data_string("Dublin"), "Dublin");
    EXPECT_EQ(wizz::http::escape_data_string(""), "");
}

TEST(HttpEscapeDataString, Reserved_and_other_ascii_characters_are_escaped)
{
    EXPECT_EQ(wizz::http::escape_data_string("!*'();:@&=$,/?#[]%\"<>\\^`{|}"),
              "%21%2A%27%28%29%3B%3A%40%26%3D%24%2C%2F%3F%23%5B%5D%25%22%3C%3E%5C%5E%60%7B%7C%7D");
    EXPECT_EQ(wizz::http::escape_data_string("\t\n"), "%09%0A");
}

TEST(HttpEscapeDataString, Every_utf8_byte_is_escaped_with_upper_case_hex)
{
    EXPECT_EQ(wizz::http::escape_data_string("São Paulo"), "S%C3%A3o%20Paulo");
    EXPECT_EQ(wizz::http::escape_data_string("Zürich–Genève"), "Z%C3%BCrich%E2%80%93Gen%C3%A8ve");
    EXPECT_EQ(wizz::http::escape_data_string("\xF0\x9F\x98\x80"), "%F0%9F%98%80");
}

} // namespace
