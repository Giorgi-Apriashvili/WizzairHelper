// decode_text: the .NET ReadAsStringAsync / File.ReadAllText decoding rules (04 §2.2, 04 §6).

#include "wizz/core/text_decoding.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using namespace std::string_literals;
using wizz::decode_text;

constexpr const char* kReplacement = "\xEF\xBF\xBD"; // U+FFFD in UTF-8

std::string replacements(int count)
{
    std::string text;
    for (int i = 0; i < count; ++i) {
        text += kReplacement;
    }
    return text;
}

TEST(TextDecoding, Well_formed_utf8_is_returned_unchanged)
{
    const std::string text = "ASCII, Kraków, Zürich, \xE2\x80\x94, \xF0\x9F\x98\x80, \xF4\x8F\xBF\xBF";
    EXPECT_EQ(decode_text(text), text);
    EXPECT_EQ(decode_text(text, "utf-8"), text);
}

TEST(TextDecoding, Each_maximal_ill_formed_utf8_subpart_becomes_one_replacement)
{
    EXPECT_EQ(decode_text("\xC3\x28"), replacements(1) + "(");          // lead byte without continuation
    EXPECT_EQ(decode_text("\xF0\x9F\x98"), replacements(1));            // truncated 4-byte sequence
    EXPECT_EQ(decode_text("a\xF0\x9F\x98" "b"), "a" + replacements(1) + "b");
    EXPECT_EQ(decode_text("\xC0\xAF"), replacements(2));                // overlong: C0 is never valid
    EXPECT_EQ(decode_text("\xE0\x80\x80"), replacements(3));            // overlong 3-byte form
    EXPECT_EQ(decode_text("\xED\xA0\x80"), replacements(3));            // encoded surrogate
    EXPECT_EQ(decode_text("\xF4\x90\x80\x80"), replacements(4));        // above U+10FFFF
    EXPECT_EQ(decode_text("\xF5\xFF\xFE"), replacements(3));            // bytes that never occur
    EXPECT_EQ(decode_text("\x80\xBF"), replacements(2));                // lone continuation bytes
    EXPECT_EQ(decode_text("\xE2\x82"), replacements(1));                // truncated at end of input
}

TEST(TextDecoding, Utf8_bom_is_stripped_once)
{
    EXPECT_EQ(decode_text("\xEF\xBB\xBF"), "");
    EXPECT_EQ(decode_text("\xEF\xBB\xBF\xEF\xBB\xBFx"), "\xEF\xBB\xBFx"); // a second BOM is a U+FEFF character
}

TEST(TextDecoding, Utf16_little_and_big_endian_with_surrogate_pairs)
{
    EXPECT_EQ(decode_text("\xFF\xFE" "A\0\xE9\0\x3D\xD8\x00\xDE"s), "A\xC3\xA9\xF0\x9F\x98\x80");
    EXPECT_EQ(decode_text("\xFE\xFF\0A\0\xE9\xD8\x3D\xDE\x00"s), "A\xC3\xA9\xF0\x9F\x98\x80");
    EXPECT_EQ(decode_text("A\0"s, "UTF-16LE"), "A");
    EXPECT_EQ(decode_text("\0A"s, "utf-16BE"), "A");
    EXPECT_EQ(decode_text("A\0"s, "unicode"), "A");
    EXPECT_EQ(decode_text("\0A"s, "unicodeFFFE"), "A");
}

TEST(TextDecoding, Utf16_unpaired_surrogates_and_odd_trailing_byte_become_replacements)
{
    EXPECT_EQ(decode_text("\xFF\xFE\x3D\xD8" "A\0"s), replacements(1) + "A");  // high surrogate + non-surrogate
    EXPECT_EQ(decode_text("\xFF\xFE\x00\xDE"s), replacements(1));             // lone low surrogate
    EXPECT_EQ(decode_text("\xFF\xFE" "A\0B"s), "A" + replacements(1));        // odd byte count
}

TEST(TextDecoding, Utf32_little_and_big_endian)
{
    EXPECT_EQ(decode_text("\xFF\xFE\0\0" "A\0\0\0\x00\xF6\x01\0"s), "A\xF0\x9F\x98\x80");
    EXPECT_EQ(decode_text("\0\0\xFE\xFF\0\0\0A\0\x01\xF6\x00"s), "A\xF0\x9F\x98\x80");
    EXPECT_EQ(decode_text("A\0\0\0"s, "utf-32"), "A");
    EXPECT_EQ(decode_text("\0\0\0A"s, "utf-32be"), "A");
}

TEST(TextDecoding, Utf32_invalid_code_points_and_partial_units_become_replacements)
{
    EXPECT_EQ(decode_text("\xFF\xFE\0\0\x00\xD8\0\0"s), replacements(1));       // surrogate code point
    EXPECT_EQ(decode_text("\xFF\xFE\0\0\x00\x00\x11\x00"s), replacements(1));   // U+110000
    EXPECT_EQ(decode_text("\xFF\xFE\0\0" "A\0\0\0B\0"s), "A" + replacements(1)); // 2 trailing bytes
}

TEST(TextDecoding, Ff_fe_without_two_zero_bytes_is_utf16_not_utf32)
{
    EXPECT_EQ(decode_text("\xFF\xFE" "A\0\0\0"s), "A"s + '\0');
}

TEST(TextDecoding, Latin1_aliases)
{
    for (const char* name : {"iso-8859-1", "ISO-8859-1", "latin1", "l1", "cp819", "csISOLatin1", "ibm819", "iso-ir-100",
                             "iso_8859-1", "iso_8859-1:1987", " latin1 "}) {
        EXPECT_EQ(decode_text("\xE9", name), "\xC3\xA9") << name;
    }
}

TEST(TextDecoding, Explicit_charset_disables_bom_detection)
{
    EXPECT_EQ(decode_text("\xFE\xFF\0A"s, "utf-8"), replacements(2) + '\0' + "A");
    EXPECT_EQ(decode_text("\xFE\xFF\0A"s, "utf-16"), "\xEF\xBF\xBE\xE4\x84\x80"); // read as LE: U+FFFE U+4100
}

TEST(TextDecoding, Unknown_and_empty_charsets_decode_as_utf8)
{
    EXPECT_EQ(decode_text("\xEF\xBB\xBFok", "klingon"), "ok");
    EXPECT_EQ(decode_text("\xC3\xA9", ""), "\xC3\xA9");
    EXPECT_EQ(decode_text("\xC3\xA9", "us-ascii"), "\xC3\xA9");
}

} // namespace
