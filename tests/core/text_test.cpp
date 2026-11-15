// Unicode text rules with .NET semantics: TextNormalizer.Key (02 §4, CT-TXT,
// P-26), encoding, classification, trimming, invariant casing and
// OrdinalIgnoreCase keys (03 §0, 07 §1).

#include "wizz/core/text.hpp"

#include <gtest/gtest.h>

#include <icu.h>

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace std::string_view_literals;
namespace text = wizz::text;
using text::UnicodeCategory;

std::string key(std::string_view input)
{
    return text::normalize_key(input);
}

// ---------------------------------------------------------------------------
// CT-TXT-01..07 (05 §3.1) — TextNormalizerTests.Key_is_accent_case_and_space_insensitive
// ---------------------------------------------------------------------------

TEST(TextNormalizer, CT_TXT_01_Accent_and_trailing_crlf_are_removed)
{
    EXPECT_EQ(key("Kraków\r\n"), "krakow");
}

TEST(TextNormalizer, CT_TXT_02_Case_and_surrounding_spaces_are_removed)
{
    EXPECT_EQ(key("  KRAKOW "), "krakow");
}

TEST(TextNormalizer, CT_TXT_03_L_with_stroke_is_mapped_after_lower_casing)
{
    EXPECT_EQ(key("Łódź"), "lodz");
}

TEST(TextNormalizer, CT_TXT_04_Space_runs_collapse_to_one)
{
    EXPECT_EQ(key("Târgu   Mureș"), "targu mures");
}

TEST(TextNormalizer, CT_TXT_05_Punctuation_is_kept)
{
    EXPECT_EQ(key("Memmingen / Munich West"), "memmingen / munich west");
}

TEST(TextNormalizer, CT_TXT_06_Empty_gives_empty)
{
    EXPECT_EQ(key(""), "");
}

TEST(TextNormalizer, CT_TXT_07_Null_gives_empty)
{
    // A default-constructed view (null data pointer) is the C++ form of a null string.
    EXPECT_EQ(key(std::string_view{}), "");
    EXPECT_EQ(text::normalize_key(std::u16string_view{}), u"");
}

// ---------------------------------------------------------------------------
// 02 §4.2 test vectors
// ---------------------------------------------------------------------------

TEST(TextNormalizer, Key_matches_every_02_4_2_vector)
{
    const std::vector<std::pair<std::string_view, std::string_view>> vectors{
        {"Kraków\r\n", "krakow"},
        {"  KRAKOW ", "krakow"},
        {"Łódź", "lodz"},
        {"Târgu   Mureș", "targu mures"},
        {"Memmingen / Munich West", "memmingen / munich west"},
        {"", ""},
        {"ŁÓDŹ", "lodz"},
        {"Đakovo", "dakovo"},
        {"Ðakovo", "ðakovo"}, // eth is not mapped
        {"Øresund", "oresund"},
        {"Straße", "strasse"},
        {"ẞ", "ss"}, // capital sharp s lower-cases to ß, then maps
        {"Æble", "aeble"},
        {"Œuvre", "oeuvre"},
        {"Iİıi", "iiii"},
        {"İstanbul", "istanbul"},
        {"Þingvellir", "thingvellir"},
        {"Reykjavík", "reykjavik"},
        {"Cluj-Napoca", "cluj-napoca"},
        {"a  b", "a b"},
        {"a\tb", "a b"},
        {"́ abc", "abc"}, // leading mark dropped, then no leading space
        {"abc ́", "abc"},
        {"Ǆ", "ǆ"},
        {"ﬁ", "ﬁ"}, // NFD does not decompose ligatures
        {"Ā", "a"},
        {"Ελλάδα", "ελλαδα"},
        {"ЁЖ", "еж"},
        {"\U00010400x", "\U00010400x"}, // supplementary characters pass through unchanged
    };
    for (const auto& [input, expected] : vectors) {
        EXPECT_EQ(key(input), expected) << "input: " << input;
    }
}

TEST(TextNormalizer, Only_non_ascii_live_station_key_keeps_the_en_dash)
{
    EXPECT_EQ(key("Palanga – Klaipeda"), "palanga – klaipeda");
}

// ---------------------------------------------------------------------------
// P-26 (05 §7.1) — letters, mapped after lower-casing
// ---------------------------------------------------------------------------

TEST(TextNormalizer, P_26_Letters_are_mapped_after_lower_casing)
{
    const std::vector<std::pair<std::string_view, std::string_view>> vectors{
        {"Straße", "strasse"},
        {"Ærø", "aero"},
        {"Œuvre", "oeuvre"},
        {"Đakovo", "dakovo"},
        {"Þórshöfn", "thorshofn"},
        {"Isparta ı", "isparta i"},
        {"A\tB C", "a b c"},
        {"İstanbul", "istanbul"},
        {"Niš", "nis"},
        {"Cluj-Napoca", "cluj-napoca"},
        {"ØRSTA", "orsta"},
    };
    for (const auto& [input, expected] : vectors) {
        EXPECT_EQ(key(input), expected) << "input: " << input;
    }
}

TEST(TextNormalizer, Key_edge_cases)
{
    EXPECT_EQ(key(" \t　  "), "");             // white space only
    EXPECT_EQ(key("́"), "");                         // only a non-spacing mark
    EXPECT_EQ(key("a ́ b"), "a b");                  // a dropped mark does not break a space run
    EXPECT_EQ(key("a​b"), "a​b");               // U+200B is not white space
    EXPECT_EQ(key("xःy"), "xःy");               // Mc (spacing mark) is kept
    EXPECT_EQ(key("x⃝y"), "x⃝y");               // Me (enclosing mark) is kept
    EXPECT_EQ(key("ÅLESUND"), "alesund");            // precomposed letter decomposes
    EXPECT_EQ(key("ÅLESUND"), "alesund");           // already decomposed
    EXPECT_EQ(key("\U00010428́"), "\U00010428");     // supplementary letter kept, mark dropped
    EXPECT_EQ(key("ŁĐØÆŒÞ"), "ldoaeoeth"); // upper-case forms are covered
}

TEST(TextNormalizer, Key_lengths_are_utf16_code_units)
{
    EXPECT_EQ(text::normalize_key(u"ß").size(), 2u); // "ß" grows to "ss"
    EXPECT_EQ(text::normalize_key(u"\U00010400").size(), 2u);
    EXPECT_EQ(text::normalize_key(u"  Kraków "), u"krakow");
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

TEST(TextEncoding, Utf8_round_trips_through_utf16)
{
    const std::string_view samples[] = {"", "BUD", "Kraków", "€ 29.99", "\U0001F308 \U00010400"};
    for (const std::string_view sample : samples) {
        EXPECT_EQ(text::utf16_to_utf8(text::utf8_to_utf16(sample)), sample);
    }
    EXPECT_EQ(text::utf8_to_utf16("\U00010400"), u"\U00010400");
}

TEST(TextEncoding, Ill_formed_utf8_becomes_one_replacement_per_maximal_subpart)
{
    EXPECT_EQ(text::utf8_to_utf16("a\x80z"), u"a�z");             // stray continuation byte
    EXPECT_EQ(text::utf8_to_utf16("a\xE2\x82z"), u"a�z");         // truncated 3-byte sequence
    EXPECT_EQ(text::utf8_to_utf16("\xE2\x82"), u"�");             // truncated at the end
    EXPECT_EQ(text::utf8_to_utf16("\xC0\xAF"), u"��");       // overlong lead byte
    EXPECT_EQ(text::utf8_to_utf16("\xE0\x80\xAF"), u"���");
    EXPECT_EQ(text::utf8_to_utf16("\xED\xA0\x80"), u"���"); // encoded surrogate
    EXPECT_EQ(text::utf8_to_utf16("\xF4\x90\x80\x80"), u"����"); // > U+10FFFF
    EXPECT_EQ(text::utf8_to_utf16("\xF0\x9F\x8C"), u"�");         // truncated 4-byte sequence
    EXPECT_EQ(text::utf8_to_utf16("\xFF"), u"�");
}

TEST(TextEncoding, Lone_surrogates_become_replacement_characters_in_utf8)
{
    const std::u16string lone_high{u'a', char16_t{0xD800}, u'b'};
    const std::u16string lone_low{char16_t{0xDC00}};
    EXPECT_EQ(text::utf16_to_utf8(lone_high), "a�b");
    EXPECT_EQ(text::utf16_to_utf8(lone_low), "�");
}

TEST(TextEncoding, Utf16_length_counts_code_units)
{
    EXPECT_EQ(text::utf16_length(""), 0u);
    EXPECT_EQ(text::utf16_length("Kraków"), 6u);
    EXPECT_EQ(text::utf16_length("ß"), 1u);
    EXPECT_EQ(text::utf16_length("\U00010400x"), 3u);
    EXPECT_EQ(text::utf16_length("\xE2\x82"), 1u); // one replacement character
}

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

TEST(TextClassification, White_space_is_exactly_the_02_4_1_set)
{
    const std::vector<char32_t> expected{0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x0020, 0x0085, 0x00A0,
                                         0x1680, 0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005, 0x2006,
                                         0x2007, 0x2008, 0x2009, 0x200A, 0x2028, 0x2029, 0x202F, 0x205F,
                                         0x3000};
    std::vector<char32_t> actual;
    for (char32_t c = 0; c <= 0x10FFFF; ++c) {
        if (text::is_white_space(c)) {
            actual.push_back(c);
        }
        // The same set as the Unicode White_Space property (ICU u_isUWhiteSpace).
        ASSERT_EQ(text::is_white_space(c), u_isUWhiteSpace(static_cast<UChar32>(c)) != 0)
            << "U+" << std::hex << static_cast<std::uint32_t>(c);
    }
    EXPECT_EQ(actual, expected);
    EXPECT_FALSE(text::is_white_space(0x200B));
    EXPECT_FALSE(text::is_white_space(0xFEFF));
    EXPECT_FALSE(text::is_white_space(0x180E));
}

TEST(TextClassification, General_category_uses_dotnet_numbering)
{
    EXPECT_EQ(static_cast<int>(UnicodeCategory::uppercase_letter), 0);
    EXPECT_EQ(static_cast<int>(UnicodeCategory::nonspacing_mark), 5);
    EXPECT_EQ(static_cast<int>(UnicodeCategory::surrogate), 16);
    EXPECT_EQ(static_cast<int>(UnicodeCategory::other_not_assigned), 29);

    EXPECT_EQ(text::general_category(U'A'), UnicodeCategory::uppercase_letter);
    EXPECT_EQ(text::general_category(U'ó'), UnicodeCategory::lowercase_letter);
    EXPECT_EQ(text::general_category(U'ǅ'), UnicodeCategory::titlecase_letter);
    EXPECT_EQ(text::general_category(U'ʰ'), UnicodeCategory::modifier_letter);
    EXPECT_EQ(text::general_category(U'あ'), UnicodeCategory::other_letter);
    EXPECT_EQ(text::general_category(U'́'), UnicodeCategory::nonspacing_mark);
    EXPECT_EQ(text::general_category(U'ः'), UnicodeCategory::spacing_combining_mark);
    EXPECT_EQ(text::general_category(U'⃝'), UnicodeCategory::enclosing_mark);
    EXPECT_EQ(text::general_category(U'7'), UnicodeCategory::decimal_digit_number);
    EXPECT_EQ(text::general_category(U'Ⅲ'), UnicodeCategory::letter_number);
    EXPECT_EQ(text::general_category(U'²'), UnicodeCategory::other_number);
    EXPECT_EQ(text::general_category(U' '), UnicodeCategory::space_separator);
    EXPECT_EQ(text::general_category(U' '), UnicodeCategory::line_separator);
    EXPECT_EQ(text::general_category(U' '), UnicodeCategory::paragraph_separator);
    EXPECT_EQ(text::general_category(U'\t'), UnicodeCategory::control);
    EXPECT_EQ(text::general_category(U'​'), UnicodeCategory::format);
    EXPECT_EQ(text::general_category(char16_t{0xD800}), UnicodeCategory::surrogate);
    EXPECT_EQ(text::general_category(U''), UnicodeCategory::private_use);
    EXPECT_EQ(text::general_category(U'_'), UnicodeCategory::connector_punctuation);
    EXPECT_EQ(text::general_category(U'-'), UnicodeCategory::dash_punctuation);
    EXPECT_EQ(text::general_category(U'('), UnicodeCategory::open_punctuation);
    EXPECT_EQ(text::general_category(U')'), UnicodeCategory::close_punctuation);
    EXPECT_EQ(text::general_category(U'“'), UnicodeCategory::initial_quote_punctuation);
    EXPECT_EQ(text::general_category(U'’'), UnicodeCategory::final_quote_punctuation);
    EXPECT_EQ(text::general_category(U'·'), UnicodeCategory::other_punctuation);
    EXPECT_EQ(text::general_category(U'+'), UnicodeCategory::math_symbol);
    EXPECT_EQ(text::general_category(U'€'), UnicodeCategory::currency_symbol);
    EXPECT_EQ(text::general_category(U'^'), UnicodeCategory::modifier_symbol);
    EXPECT_EQ(text::general_category(U'©'), UnicodeCategory::other_symbol);
    EXPECT_EQ(text::general_category(U'͸'), UnicodeCategory::other_not_assigned);
}

TEST(TextClassification, Letters_and_digits_follow_dotnet_char)
{
    for (const char32_t letter : {U'a', U'Z', U'Ł', U'ǅ', U'ʰ', U'あ', U'\U00010400'}) {
        EXPECT_TRUE(text::is_letter(letter));
        EXPECT_TRUE(text::is_letter_or_digit(letter));
        EXPECT_FALSE(text::is_digit(letter));
    }
    for (const char32_t digit : {U'0', U'9', U'٣'}) {
        EXPECT_TRUE(text::is_digit(digit));
        EXPECT_TRUE(text::is_letter_or_digit(digit));
        EXPECT_FALSE(text::is_letter(digit));
    }
    // Typical PrefixMatches boundaries (02 §5.7) and other non-alphanumerics.
    for (const char32_t other : {U' ', U'/', U'(', U'-', U'_', U'́', U'²', U'Ⅲ', char32_t{0xD800}}) {
        EXPECT_FALSE(text::is_letter_or_digit(other));
        EXPECT_FALSE(text::is_letter(other));
        EXPECT_FALSE(text::is_digit(other));
    }
}

TEST(TextClassification, Letter_or_digit_equals_icu_u_isalnum_on_the_bmp)
{
    for (char32_t c = 0; c <= 0xFFFF; ++c) {
        ASSERT_EQ(text::is_letter_or_digit(c), u_isalnum(static_cast<UChar32>(c)) != 0)
            << "U+" << std::hex << static_cast<std::uint32_t>(c);
    }
}

// ---------------------------------------------------------------------------
// Trimming
// ---------------------------------------------------------------------------

TEST(TextTrim, Trims_dotnet_white_space_at_both_ends)
{
    EXPECT_EQ(text::trim("  KRAKOW "), "KRAKOW");
    EXPECT_EQ(text::trim("Kraków\r\n"), "Kraków");
    EXPECT_EQ(text::trim(" 　\u0085x y   "), "x y");
    EXPECT_EQ(text::trim("​x​"), "​x​"); // zero width space is not white space
    EXPECT_EQ(text::trim(" \t\r\n "), "");
    EXPECT_EQ(text::trim(""), "");
    EXPECT_EQ(text::trim_start("  a  "), "a  ");
    EXPECT_EQ(text::trim_end("  a  "), "  a");
    EXPECT_EQ(text::trim_end("a "), "a");
    EXPECT_EQ(text::trim_end("é"), "é"); // last byte 0xA9 is not a white-space suffix
}

TEST(TextTrim, Ill_formed_bytes_are_not_white_space)
{
    EXPECT_EQ(text::trim(" \x80 "), "\x80");
    EXPECT_EQ(text::trim(" \xE2\x80 "), "\xE2\x80");
    EXPECT_EQ(text::trim_end("\xF0\xE2\x80\x80"), "\xF0"); // U+2000 after a lone lead byte is trimmed
}

TEST(TextTrim, Returns_a_view_into_the_input)
{
    const std::string input = "  BUD ";
    const std::string_view trimmed = text::trim(input);
    EXPECT_EQ(trimmed.data(), input.data() + 2);
    EXPECT_EQ(trimmed.size(), 3u);
}

TEST(TextTrim, Utf16_overloads)
{
    EXPECT_EQ(text::trim(u"  x　"), u"x");
    EXPECT_EQ(text::trim_start(u"\t x "), u"x ");
    EXPECT_EQ(text::trim_end(u"\t x "), u"\t x");
}

TEST(TextTrim, Is_null_or_white_space)
{
    EXPECT_TRUE(text::is_null_or_white_space(std::string_view{}));
    EXPECT_TRUE(text::is_null_or_white_space(""));
    EXPECT_TRUE(text::is_null_or_white_space(" \t\r\n 　"));
    EXPECT_FALSE(text::is_null_or_white_space("​"));
    EXPECT_FALSE(text::is_null_or_white_space(" x "));
    EXPECT_TRUE(text::is_null_or_white_space(u" "));
    EXPECT_FALSE(text::is_null_or_white_space(u"."));
}

// ---------------------------------------------------------------------------
// Invariant casing
// ---------------------------------------------------------------------------

TEST(TextCasing, Invariant_upper_and_lower_case_are_simple_mappings)
{
    EXPECT_EQ(text::to_upper_invariant("bud"), "BUD");
    EXPECT_EQ(text::to_upper_invariant("huf"), "HUF");
    EXPECT_EQ(text::to_upper_invariant("istanbul"), "ISTANBUL");
    EXPECT_EQ(text::to_upper_invariant("straße"), "STRAßE"); // no expansion to SS
    EXPECT_EQ(text::to_upper_invariant("ςǆÿ"), "ΣǄŸ");
    EXPECT_EQ(text::to_lower_invariant("LONDON"), "london");
    EXPECT_EQ(text::to_lower_invariant("ǄЁẞ"), "ǆёß");
}

TEST(TextCasing, Invariant_casing_keeps_dotless_and_dotted_i)
{
    EXPECT_EQ(text::to_upper_invariant("ı"), "ı");
    EXPECT_EQ(text::to_lower_invariant("İ"), "İ");
    EXPECT_EQ(text::to_upper_invariant(char16_t{0x0131}), char16_t{0x0131});
    EXPECT_EQ(text::to_lower_invariant(char16_t{0x0130}), char16_t{0x0130});
    EXPECT_EQ(text::to_lower_invariant("I"), "i");
}

TEST(TextCasing, Per_unit_overloads_leave_surrogates_alone)
{
    EXPECT_EQ(text::to_lower_invariant(u'A'), u'a');
    EXPECT_EQ(text::to_upper_invariant(u'é'), u'É');
    EXPECT_EQ(text::to_lower_invariant(char16_t{0xD801}), char16_t{0xD801});
    EXPECT_EQ(text::to_upper_invariant(char16_t{0xDC28}), char16_t{0xDC28});
}

TEST(TextCasing, String_overloads_map_supplementary_code_points)
{
    // string.ToLowerInvariant / ToUpperInvariant on .NET-on-ICU walk code points.
    EXPECT_EQ(text::to_lower_invariant("\U00010400"), "\U00010428");
    EXPECT_EQ(text::to_upper_invariant("\U00010428"), "\U00010400");
}

// ---------------------------------------------------------------------------
// OrdinalIgnoreCase
// ---------------------------------------------------------------------------

TEST(TextOrdinalIgnoreCase, Equality_uses_simple_upper_case)
{
    EXPECT_TRUE(text::ordinal_ignore_case_equals("bud", "BUD"));
    EXPECT_TRUE(text::ordinal_ignore_case_equals("bUd", "Bud"));
    EXPECT_TRUE(text::ordinal_ignore_case_equals("", ""));
    EXPECT_TRUE(text::ordinal_ignore_case_equals("Kraków", "KRAKÓW")); // not ASCII-only
    EXPECT_TRUE(text::ordinal_ignore_case_equals("\U00010428", "\U00010400"));
    EXPECT_FALSE(text::ordinal_ignore_case_equals("Kraków", "Krakow")); // accents count
    EXPECT_FALSE(text::ordinal_ignore_case_equals("ı", "I"));           // .NET OrdinalCasing exception
    EXPECT_FALSE(text::ordinal_ignore_case_equals("ſ", "S"));           // .NET OrdinalCasing exception
    EXPECT_FALSE(text::ordinal_ignore_case_equals("i", "İ"));
    EXPECT_FALSE(text::ordinal_ignore_case_equals("ß", "SS"));
    EXPECT_FALSE(text::ordinal_ignore_case_equals("BUD", "BUD "));
}

TEST(TextOrdinalIgnoreCase, Ordering_compares_upper_cased_code_points)
{
    EXPECT_LT(text::ordinal_ignore_case_compare("abc", "ABD"), 0);
    EXPECT_GT(text::ordinal_ignore_case_compare("b", "A"), 0);
    EXPECT_LT(text::ordinal_ignore_case_compare("lon", "LONDON"), 0);
    EXPECT_EQ(text::ordinal_ignore_case_compare("LTN", "ltn"), 0);
    EXPECT_GT(text::ordinal_ignore_case_compare("_", "a"), 0); // 'a' compares as 'A' (0x41) < '_' (0x5F)
    // Code-point order (verified on .NET 10), unlike case-sensitive CompareOrdinal.
    EXPECT_LT(text::ordinal_ignore_case_compare("", "\U00010400"), 0);
    EXPECT_LT(text::ordinal_ignore_case_compare("ａ", "\U00010428"), 0);
}

TEST(TextOrdinalIgnoreCase, Matches_dotnet_10_sign_matrix)
{
    // string.Compare(row, column, StringComparison.OrdinalIgnoreCase) signs,
    // recorded with .NET 10.0.15 (ICU globalization) on Windows 11.
    const std::vector<std::string_view> samples{
        "a", "A", "é", "É", "ı", "I", "i", "İ", "ſ", "s", "S", "ß", "ẞ",
        "ǅ", "ǆ", "Ǆ", "\U00010400", "\U00010428", "", "�", "µ", "Μ",
        "ÿ", "Ÿ", "k", "K", "Ω", "ω", "Ω", "aa", "AB", ""};
    const std::vector<std::string_view> expected{
        "00-----------------------------+", "00-----------------------------+",
        "++00-++--++-------------+----+++", "++00-++--++-------------+----+++",
        "++++0+++-+++------------+----+++", "++---00----------------------+++",
        "++---00----------------------+++", "++++-++0-+++------------+----+++",
        "++++++++0+++----------+++----+++", "++---++--00-------------+----+++",
        "++---++--00-------------+----+++", "++++-++--++0------------+----+++",
        "++++++++++++0+++----+++++--+++++", "++++++++++++-000------+++----+++",
        "++++++++++++-000------+++----+++", "++++++++++++-000------+++----+++",
        "++++++++++++++++00++++++++++++++", "++++++++++++++++00++++++++++++++",
        "++++++++++++++++--0-++++++++++++", "++++++++++++++++--+0++++++++++++",
        "++++++++++++-+++----00+++----+++", "++++++++++++-+++----00+++----+++",
        "++++++++-+++----------00+----+++", "++++++++-+++----------00+----+++",
        "++---++-----------------0----+++", "++++++++++++++++----+++++0++++++",
        "++++++++++++++++----+++++-0+++++", "++++++++++++-+++----+++++--00+++",
        "++++++++++++-+++----+++++--00+++", "++---------------------------0-+",
        "++---------------------------+0+", "-------------------------------0"};
    ASSERT_EQ(expected.size(), samples.size());
    for (std::size_t row = 0; row < samples.size(); ++row) {
        std::string signs;
        for (const std::string_view column : samples) {
            const int result = text::ordinal_ignore_case_compare(samples[row], column);
            signs += result < 0 ? '-' : result > 0 ? '+' : '0';
            EXPECT_EQ(result == 0, text::ordinal_ignore_case_equals(samples[row], column));
            if (result == 0) {
                EXPECT_EQ(text::OrdinalIgnoreCaseHash{}(samples[row]), text::OrdinalIgnoreCaseHash{}(column));
            }
        }
        EXPECT_EQ(signs, expected[row]) << "row " << row << ": " << samples[row];
    }
}

TEST(TextOrdinalIgnoreCase, Hash_and_predicates_key_code_containers)
{
    const text::OrdinalIgnoreCaseHash hash;
    EXPECT_EQ(hash("lon"), hash("LON"));
    EXPECT_EQ(hash("Kraków"), hash("KRAKÓW"));
    EXPECT_NE(hash("LON"), hash("LTN"));

    std::unordered_map<std::string, int, text::OrdinalIgnoreCaseHash, text::OrdinalIgnoreCaseEqual> by_iata{
        {"BUD", 1}, {"LTN", 2}};
    EXPECT_EQ(by_iata.find("bud"sv)->second, 1); // heterogeneous lookup, no allocation
    EXPECT_EQ(by_iata.count("lTn"), 1u);
    EXPECT_FALSE(by_iata.emplace("ltn", 3).second); // a case variant is the same key

    std::map<std::string, int, text::OrdinalIgnoreCaseLess> ordered{{"waw", 1}, {"BUD", 2}, {"ltn", 3}};
    std::vector<std::string> keys;
    for (const auto& [code, value] : ordered) {
        keys.push_back(code);
    }
    EXPECT_EQ(keys, (std::vector<std::string>{"BUD", "ltn", "waw"}));
    EXPECT_TRUE(ordered.contains("LTN"sv));
}

} // namespace
