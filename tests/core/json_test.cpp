// Strict JSON reader / writer with System.Text.Json semantics
// (02 §5.1, 02 §6.9, 03 §7.2, 04 §9.3, 04 §9.4, 01 §13, 01 §17.3).

#include "wizz/core/json.hpp"

#include <gtest/gtest.h>

#include <clocale>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace json = wizz::json;
using namespace std::string_view_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// The decoded text of a JSON string document.
std::string decoded(std::string_view text)
{
    const json::Value value = json::parse(text);
    return std::string{value.as_string()};
}

/// The lexeme of a JSON number document.
std::string lexeme_of(std::string_view text)
{
    const json::Value value = json::parse(text);
    return std::string{value.number_lexeme()};
}

/// Message of the ParseError that `text` must raise.
std::string parse_error_message(std::string_view text)
{
    try {
        (void)json::parse(text);
    } catch (const json::ParseError& error) {
        return error.what();
    }
    ADD_FAILURE() << "parsed without error";
    return {};
}

void expect_all_rejected(std::initializer_list<std::string_view> texts)
{
    for (const std::string_view text : texts) {
        SCOPED_TRACE(testing::Message() << "input: " << testing::PrintToString(std::string{text}));
        EXPECT_THROW((void)json::parse(text), json::ParseError);
    }
}

/// Message of the TypeError that `access` must raise.
template <typename Access>
std::string type_error_message(Access&& access)
{
    try {
        std::forward<Access>(access)();
    } catch (const json::TypeError& error) {
        return error.what();
    }
    ADD_FAILURE() << "no TypeError";
    return {};
}

std::string nested_arrays(int depth, std::string_view innermost = "")
{
    return std::string(static_cast<std::size_t>(depth), '[') + std::string{innermost} +
           std::string(static_cast<std::size_t>(depth), ']');
}

std::string nested_objects(int depth)
{
    std::string text;
    for (int i = 1; i < depth; ++i) {
        text += R"({"a":)";
    }
    text += "{}";
    text += std::string(static_cast<std::size_t>(depth - 1), '}');
    return text;
}

/// The JSON text of one string value.
std::string quoted(std::string_view text)
{
    return json::Writer{}.string(text).take();
}

std::string crlf_lines(std::initializer_list<std::string_view> lines)
{
    std::string text;
    for (const std::string_view line : lines) {
        if (!text.empty()) {
            text += "\r\n";
        }
        text += line;
    }
    return text;
}

/// 05 §1.7: fixtures are copied to <test-binary-dir>/Fixtures.
std::filesystem::path fixtures_dir()
{
    const auto beside_binary = std::filesystem::path{testing::internal::GetArgvs()[0]}.parent_path() / "Fixtures";
    return std::filesystem::is_directory(beside_binary) ? beside_binary : std::filesystem::path{"Fixtures"};
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

json::Value parse_fixture(std::string_view name)
{
    return json::parse(read_file(fixtures_dir() / name));
}

/// Calls a view-returning accessor on an expression of type T.
template <typename T>
concept CanFind = requires(T&& value) { std::forward<T>(value).find("x"); };
template <typename T>
concept CanViewString = requires(T&& value) { std::forward<T>(value).as_string(); };

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

static_assert(std::is_base_of_v<std::runtime_error, json::Error>);
static_assert(std::is_base_of_v<json::Error, json::ParseError>);
static_assert(std::is_base_of_v<json::Error, json::TypeError>);
static_assert(std::is_base_of_v<json::Error, json::FormatError>);
static_assert(std::is_nothrow_move_constructible_v<json::Value>);
static_assert(std::is_nothrow_move_assignable_v<json::Value>);
static_assert(std::is_copy_constructible_v<json::Value>);
// View accessors are unavailable on temporaries, so views cannot dangle.
static_assert(CanFind<const json::Value&>);
static_assert(!CanFind<json::Value>);
static_assert(CanViewString<json::Value&>);
static_assert(!CanViewString<const json::Value>);

// ---------------------------------------------------------------------------
// Reader: accepted documents
// ---------------------------------------------------------------------------

TEST(JsonReader, Parses_every_scalar_at_the_root)
{
    EXPECT_TRUE(json::parse("null").is_null());
    EXPECT_TRUE(json::parse("true").as_bool());
    EXPECT_FALSE(json::parse("false").as_bool());
    EXPECT_EQ(lexeme_of("0"), "0");
    EXPECT_EQ(decoded(R"("x")"), "x");
    EXPECT_EQ(json::parse("[]").kind(), json::Kind::array);
    EXPECT_EQ(json::parse("{}").kind(), json::Kind::object);
}

TEST(JsonReader, Accepts_SP_HT_LF_CR_white_space_everywhere)
{
    const json::Value root = json::parse(" \t\r\n{ \"a\" \t:\r\n [ 1 ,\n2\t] ,\"b\":{ } } \r\n\t ");
    ASSERT_TRUE(root.is_object());
    ASSERT_EQ(root.find("a")->as_array().size(), 2u);
    EXPECT_EQ(root.find("a")->at(1).number_lexeme(), "2");
    EXPECT_TRUE(root.find("b")->as_object().empty());
}

TEST(JsonReader, Kind_queries_match_exactly_one_kind)
{
    const struct {
        std::string_view text;
        json::Kind kind;
    } cases[] = {
        {"null", json::Kind::null},     {"true", json::Kind::boolean},    {"false", json::Kind::boolean},
        {"-1.5e3", json::Kind::number}, {R"("s")", json::Kind::string}, {"[1]", json::Kind::array},
        {R"({"a":1})", json::Kind::object},
    };
    for (const auto& [text, kind] : cases) {
        SCOPED_TRACE(text);
        const json::Value value = json::parse(text);
        EXPECT_EQ(value.kind(), kind);
        EXPECT_EQ(value.is_null(), kind == json::Kind::null);
        EXPECT_EQ(value.is_bool(), kind == json::Kind::boolean);
        EXPECT_EQ(value.is_number(), kind == json::Kind::number);
        EXPECT_EQ(value.is_string(), kind == json::Kind::string);
        EXPECT_EQ(value.is_array(), kind == json::Kind::array);
        EXPECT_EQ(value.is_object(), kind == json::Kind::object);
    }
    EXPECT_TRUE(json::Value{}.is_null());
}

TEST(JsonReader, Is_true_only_for_the_literal_true)
{
    // 02 §5.1: "isFakeStation":"true" (a string) is false.
    EXPECT_TRUE(json::parse("true").is_true());
    EXPECT_FALSE(json::parse("false").is_true());
    EXPECT_FALSE(json::parse(R"("true")").is_true());
    EXPECT_FALSE(json::parse("1").is_true());
    EXPECT_FALSE(json::parse("null").is_true());
}

// ---------------------------------------------------------------------------
// Reader: strictness (RFC 8259, JsonDocument defaults)
// ---------------------------------------------------------------------------

TEST(JsonReader, Rejects_empty_and_white_space_only_input)
{
    expect_all_rejected({"", " ", "\r\n\t "});
    EXPECT_TRUE(parse_error_message("").starts_with("The input does not contain any JSON tokens."));
}

TEST(JsonReader, Rejects_comments)
{
    expect_all_rejected({"// c\n1", "/* c */ 1", "1 // c", "1 /* c */", "[1 /* c */]", "{\"a\":1 // c\n}",
                         "{/* c */}", "# c\n1"});
}

TEST(JsonReader, Rejects_trailing_commas)
{
    expect_all_rejected({"[1,]", "[1, ]", "[1,\n]", "{\"a\":1,}", "{\"a\":1 ,\n}", "[,]", "[,1]", "[1,,2]", "{,}",
                         "{\"a\":1,,\"b\":2}", "[[],]", "{\"a\":{},}"});
    EXPECT_EQ(parse_error_message("[1,]"), "The JSON array contains a trailing comma at the end which is not "
                                           "supported in this mode. LineNumber: 0 | BytePositionInLine: 3.");
    EXPECT_EQ(parse_error_message("{\n  \"a\": 1,\n}"),
              "The JSON object contains a trailing comma at the end which is not supported in this mode. "
              "LineNumber: 2 | BytePositionInLine: 0.");
}

TEST(JsonReader, Rejects_leading_plus_and_leading_zeros)
{
    expect_all_rejected({"+1", "[+1]", "{\"a\":+1}", "+0", "01", "-01", "00", "00.5", "-00", "[01]", "{\"a\":007}"});
    EXPECT_EQ(parse_error_message("01"), "Invalid leading zero before '1'. LineNumber: 0 | BytePositionInLine: 1.");
}

TEST(JsonReader, Rejects_NaN_and_Infinity)
{
    expect_all_rejected({"NaN", "-NaN", "nan", "Infinity", "-Infinity", "+Infinity", "inf", "[NaN]", "{\"a\":Infinity}"});
}

TEST(JsonReader, Rejects_malformed_numbers)
{
    expect_all_rejected({"-", "-a", "- 1", "--1", ".5", "-.5", "1.", "1.e5", "1e", "1E+", "1e-", "1e+-1", "0x1F", "1_000",
                         "1.5.5", "1e5.5", "1,5", "[1.]", "[-]", "1ee5"});
}

TEST(JsonReader, Accepts_the_full_number_grammar)
{
    for (const std::string_view text : {"0", "-0", "0.0", "-0.0", "0e0", "0E+0", "1e-0", "10", "123456789", "-12.50e-3",
                                        "1.5E+2", "9007199254740993", "123456789012345678901234567890"}) {
        SCOPED_TRACE(text);
        EXPECT_EQ(lexeme_of(text), text);
    }
}

TEST(JsonReader, White_space_is_only_SP_HT_LF_CR)
{
    // Vertical tab, form feed, NBSP, a byte order mark and U+2000 are not JSON white space.
    expect_all_rejected({"\v1", "\f1", "1\v", "[1,\f2]", "\xC2\xA0" "1", "1\xC2\xA0", "\xEF\xBB\xBF{}",
                         "\xE2\x80\x80" "1"});
    EXPECT_EQ(parse_error_message("\xEF\xBB\xBF{}"),
              "'0xEF' is an invalid start of a value. LineNumber: 0 | BytePositionInLine: 0.");
}

TEST(JsonReader, Rejects_misspelled_or_unknown_literals)
{
    expect_all_rejected({"True", "FALSE", "Null", "nul", "tru", "fals", "nulll", "truefalse", "undefined",
                         "[true false]", "[t]", "None"});
}

TEST(JsonReader, Rejects_any_content_after_the_root_value)
{
    expect_all_rejected({"{} {}", "1 2", "{}x", "null,", R"("a""b")", "[] []", "true false", "0 0", "{}}", "[]]",
                         std::string_view{"{}\0", 3}, "1/", "\"a\" // c"});
    EXPECT_EQ(parse_error_message("1 2"),
              "'2' is invalid after a single JSON value. Expected end of data. LineNumber: 0 | BytePositionInLine: 2.");
}

TEST(JsonReader, Rejects_broken_structure)
{
    expect_all_rejected({"{", "[", "[1", "[1,", "{\"a\"", "{\"a\":", "{\"a\":1", "{\"a\" 1}", "{\"a\":1 \"b\":2}",
                         "[1 2]", "]", "}", "[}", "{]", ":", ",", "{\"a\"::1}", "{\"a\":1:2}", "[1:2]"});
}

TEST(JsonReader, Property_names_must_be_double_quoted_strings)
{
    expect_all_rejected({"{a:1}", "{'a':1}", "{1:2}", "{null:1}", "{true:1}", "{[]:1}", "{{}:1}"});
}

TEST(JsonReader, Rejects_single_quoted_and_unterminated_strings)
{
    expect_all_rejected({"'a'", "\"abc", "\"abc\\\"", "[\"a]", "{\"a\":\"b}"});
    EXPECT_EQ(parse_error_message("\"abc"),
              "Expected end of string, but instead reached end of data. LineNumber: 0 | BytePositionInLine: 4.");
}

TEST(JsonReader, Rejects_unescaped_control_characters_in_strings)
{
    for (int c = 0; c < 0x20; ++c) {
        const std::string text = std::string{"\"a"} + static_cast<char>(c) + "b\"";
        SCOPED_TRACE(c);
        EXPECT_THROW((void)json::parse(text), json::ParseError);
    }
    // DEL (U+007F) is not a control character for RFC 8259 and may appear raw.
    EXPECT_EQ(decoded("\"a\x7F\""), "a\x7F");
}

TEST(JsonReader, Rejects_invalid_escapes)
{
    expect_all_rejected({R"("\x")", R"("\'")", R"("\U0041")", R"("\u12")", R"("\u12G4")", R"("\u")", "\"\\",
                         R"("\a")", R"("\0")", R"("\v")", R"("\ ")", R"("\u 123")"});
}

// D-28: JsonDocument.Parse accepts unpaired surrogate escapes (RFC 8259 allows them); only
// reading that text throws InvalidOperationException (verified on .NET 10). Escapes are
// spelled "\\u" in ordinary literals here so that every input is visibly literal.
constexpr std::string_view kIncompleteUtf16 =
    "Cannot read incomplete UTF-16 JSON text as string with missing low surrogate.";

std::string invalid_utf16(std::string_view unit)
{
    return std::format("Cannot read invalid UTF-16 JSON text as string. Invalid surrogate value: '{}'.", unit);
}

TEST(JsonReader, Lone_surrogate_escapes_parse_but_their_text_cannot_be_read)
{
    const struct {
        std::string_view text;
        std::string message;
    } cases[] = {
        {"\"\\uD800\"", std::string{kIncompleteUtf16}},                    // high surrogate at the end
        {"\"ab\\ud83d\"", std::string{kIncompleteUtf16}},                  // lower-case hex
        {"\"\\uD800x\"", std::string{kIncompleteUtf16}},                   // followed by a character
        {"\"\\uD800\\n\"", std::string{kIncompleteUtf16}},                 // followed by another escape
        {"\"\\uD83D\\uDE00\\uD800\"", std::string{kIncompleteUtf16}},      // after a valid pair
        {"\"\\uDC00\"", invalid_utf16("0xDC00")},                         // a low surrogate first
        {"\"x\\udfff\"", invalid_utf16("0xDFFF")},                        //
        {"\"\\uDBFF\\u0041\"", invalid_utf16("0x41")},                    // high + an escape that is not low: that unit (X2)
        {"\"\\uD800\\uE000\"", invalid_utf16("0xE000")},                  //
        {"\"\\uD83D\\uD83D\"", invalid_utf16("0xD83D")},                  // two high surrogates
        {"\"\\uD800\\uD83D\\uDE00\"", invalid_utf16("0xD83D")},           //
        {"\"\\uDE00\\uD83D\"", invalid_utf16("0xDE00")},                  // the first problem wins
        {"\"x\\uDC00\\uD800\"", invalid_utf16("0xDC00")},                 //
    };
    for (const auto& [text, message] : cases) {
        SCOPED_TRACE(text);
        const json::Value value = json::parse(text);
        EXPECT_EQ(value.kind(), json::Kind::string);
        EXPECT_TRUE(value.is_string());
        EXPECT_EQ(type_error_message([&] { (void)value.as_string(); }), message);
    }
}

TEST(JsonReader, Lone_surrogates_leave_the_rest_of_the_document_usable)
{
    const json::Value root =
        json::parse("{\"aliases\":[\"\\ud83d\",\"ok\"],\"county\":\"\\udc00\",\"name\":\"Graz\",\"n\":5}");
    EXPECT_EQ(root.get_string("name"), "Graz");
    EXPECT_EQ(root.find("n")->number_lexeme(), "5");
    const json::Value& aliases = *root.find("aliases");
    ASSERT_EQ(aliases.as_array().size(), 2u);
    EXPECT_EQ(aliases.at(1).as_string(), "ok");
    EXPECT_EQ(type_error_message([&] { (void)aliases.at(0).as_string(); }), kIncompleteUtf16);

    // get_string reads the text, so it throws like GetString; other accessors see a String.
    EXPECT_EQ(type_error_message([&] { (void)root.get_string("county"); }), invalid_utf16("0xDC00"));
    const json::Value& county = *root.find("county");
    EXPECT_EQ(type_error_message([&] { (void)county.as_bool(); }),
              "The requested operation requires an element of type 'Boolean', but the target element has type "
              "'String'.");
    EXPECT_EQ(county.try_int32(), std::nullopt);
    EXPECT_FALSE(county.is_true());

    const json::Value copy = root;
    EXPECT_THROW((void)copy.get_string("county"), json::TypeError);
    EXPECT_EQ(copy.get_string("name"), "Graz");
}

TEST(JsonReader, Lone_surrogate_in_a_member_name_fails_the_lookups_that_reach_it)
{
    // find / get_string scan from the last member to the first (like TryGetProperty) and throw
    // when they reach the unreadable name before a match; Member::name throws like JsonProperty.Name.
    const json::Value root = json::parse("{\"x\":1,\"\\ud800\":2,\"y\":3}");
    EXPECT_EQ(root.find("y")->number_lexeme(), "3");
    EXPECT_EQ(type_error_message([&] { (void)root.find("x"); }), kIncompleteUtf16);
    EXPECT_THROW((void)root.find("missing"), json::TypeError);
    EXPECT_THROW((void)root.get_string("x"), json::TypeError);

    const auto members = root.as_object();
    ASSERT_EQ(members.size(), 3u);
    EXPECT_EQ(members[0].name(), "x");
    EXPECT_EQ(type_error_message([&] { (void)members[1].name(); }), kIncompleteUtf16);
    EXPECT_EQ(members[1].value().number_lexeme(), "2");
    EXPECT_EQ(members[2].name(), "y");

    const json::Value low = json::parse("{\"a\\uDC00\":true}");
    EXPECT_EQ(type_error_message([&] { (void)low.find("a"); }), invalid_utf16("0xDC00"));
}

TEST(JsonReader, Surrogate_escapes_keep_the_strict_escape_grammar)
{
    // Only the pairing is relaxed: malformed or truncated escapes and encoded surrogates still fail.
    expect_all_rejected({"\"\\uD800\\u12G4\"", "\"\\uD800\\u\"", "\"\\uD800\\u12\"", "\"\\uD800\\\"",
                         "\"\\uD800\\x\"", "\"\\uDC00", "[\"\\uD800\",]", "{\"\\uD800\"}", "\"\xED\xA0\x80\""});
}

TEST(JsonReader, Rejects_ill_formed_UTF8)
{
    expect_all_rejected({
        "\"\x80\"",              // lone continuation byte
        "\"\xBF\"",              //
        "\"\xC0\xAF\"",          // overlong '/'
        "\"\xC1\xBF\"",          // overlong
        "\"\xE0\x80\xAF\"",      // overlong 3-byte
        "\"\xF0\x80\x80\xAF\"",  // overlong 4-byte
        "\"\xED\xA0\x80\"",      // encoded surrogate U+D800
        "\"\xED\xBF\xBF\"",      // encoded surrogate U+DFFF
        "\"\xF4\x90\x80\x80\"",  // above U+10FFFF
        "\"\xF5\x80\x80\x80\"",  // invalid lead
        "\"\xFE\"",              //
        "\"\xFF\"",              //
        "\"\xC3\"",              // truncated 2-byte before the closing quote
        "\"\xE2\x82\"",          // truncated 3-byte
        "\"\xF0\x9F\x98\"",      // truncated 4-byte
        "\"\xC3",                // truncated at end of input
        "\xC3\xA9",              // non-ASCII outside a string
    });
}

TEST(JsonReader, Accepts_well_formed_raw_UTF8)
{
    // Boundaries of every sequence length, plus the map fixture's raw "Kraków".
    const std::string text = "\x7F\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80"
                             "\xF4\x8F\xBF\xBF Krak\xC3\xB3w \xE2\x82\xAC \xF0\x9F\x98\x80";
    EXPECT_EQ(decoded("\"" + text + "\""), text);
}

TEST(JsonReader, Decodes_simple_escapes)
{
    EXPECT_EQ(decoded(R"("\"\\\/\b\f\n\r\t")"), "\"\\/\b\f\n\r\t");
}

TEST(JsonReader, Decodes_unicode_escapes_to_UTF8)
{
    const struct {
        std::string_view text;
        std::string_view utf8;
    } cases[] = {
        {R"("\u0041\u00e9\u00C9")", "A\xC3\xA9\xC3\x89"},                  // hex digits in either case
        {R"("\u2013")", "\xE2\x80\x93"},                                    // EN DASH (02 §6.9)
        {R"("\u2192\u201C")", "\xE2\x86\x92\xE2\x80\x9C"},                  // → “ (01 §17.3)
        {R"("\uD83D\uDE00")", "\xF0\x9F\x98\x80"},                          // surrogate pair U+1F600
        {R"("\uD800\uDC00")", "\xF0\x90\x80\x80"},                          // U+10000
        {R"("\uDBFF\uDFFF")", "\xF4\x8F\xBF\xBF"},                          // U+10FFFF
        {R"("\u007F\u0080\u07FF\u0800\uFFFF")", "\x7F\xC2\x80\xDF\xBF\xE0\xA0\x80\xEF\xBF\xBF"},
        {R"("\uD7FF\uE000")", "\xED\x9F\xBF\xEE\x80\x80"},                  // just outside the surrogate block
        {R"("\u0000")", std::string_view{"\0", 1}},                         // NUL survives
        {R"("x\u0022y\u005C")", "x\"y\\"},
    };
    for (const auto& [text, utf8] : cases) {
        SCOPED_TRACE(text);
        EXPECT_EQ(decoded(text), utf8);
    }
}

TEST(JsonReader, Decodes_the_Krakow_name_in_both_spellings)
{
    // map_subset.json carries raw "ó" with escaped CR LF (05 §2.2, CT-TXT-01 input); .NET writes "\u00F3".
    constexpr std::string_view kExpected = "Krak\xC3\xB3w\r\n";
    EXPECT_EQ(decoded("\"Krak\xC3\xB3w\\r\\n\""), kExpected);
    EXPECT_EQ(decoded(R"("Krak\u00F3w\r\n")"), kExpected);
}

// ---------------------------------------------------------------------------
// Reader: depth limit (JsonReaderOptions.MaxDepth = 64)
// ---------------------------------------------------------------------------

TEST(JsonReader, Accepts_exactly_64_nested_containers)
{
    EXPECT_NO_THROW((void)json::parse(nested_arrays(64)));
    EXPECT_NO_THROW((void)json::parse(nested_arrays(64, "1")));
    EXPECT_NO_THROW((void)json::parse(nested_objects(64)));
    EXPECT_NO_THROW((void)json::parse(nested_arrays(63, "{\"a\":1}")));
    // Depth is released when a container closes: siblings may each reach the limit.
    EXPECT_NO_THROW((void)json::parse("[" + nested_arrays(63) + "," + nested_arrays(63) + "]"));
}

TEST(JsonReader, Rejects_a_65th_nested_container)
{
    expect_all_rejected({nested_arrays(65), nested_arrays(65, "1"), nested_objects(65), nested_arrays(64, "{}"),
                         nested_arrays(63, R"({"a":[]})")});
    EXPECT_EQ(parse_error_message(nested_arrays(65)),
              "CurrentDepth (64) is equal to or larger than the maximum allowed depth of 64. Cannot read next JSON "
              "array. LineNumber: 0 | BytePositionInLine: 64.");
    EXPECT_TRUE(parse_error_message(nested_objects(65)).starts_with("CurrentDepth (64)"));
}

TEST(JsonReader, Very_deep_input_fails_without_exhausting_the_stack)
{
    EXPECT_THROW((void)json::parse(std::string(100'000, '[')), json::ParseError);
    EXPECT_THROW((void)json::parse(nested_arrays(100'000)), json::ParseError);
}

TEST(JsonReader, Parses_a_two_megabyte_document_in_one_pass)
{
    std::string text = "{\"outboundFlights\":[";
    constexpr int kRows = 16'000;
    for (int i = 0; i < kRows; ++i) {
        text += std::format(R"({}{{"departureStation":"BUD","arrivalStation":"LTN","departureDate":"2026-11-10T06:00:00",)"
                            R"("price":{{"amount":{}.0,"currencyCode":"HUF"}},"name":"Krak\u00F3w","hasMacFlight":false}})",
                            i == 0 ? "" : ",", 30'000 + i);
    }
    text += "],\"returnFlights\":null}";
    ASSERT_GT(text.size(), 2'000'000u);

    const json::Value root = json::parse(text);
    const auto rows = root.find("outboundFlights")->as_array();
    ASSERT_EQ(rows.size(), static_cast<std::size_t>(kRows));
    EXPECT_EQ(rows.back().find("price")->find("amount")->number_lexeme(), "45999.0");
    EXPECT_EQ(rows.back().get_string("name"), "Krak\xC3\xB3w");
}

// ---------------------------------------------------------------------------
// Reader: error messages and positions
// ---------------------------------------------------------------------------

TEST(JsonReader, Errors_report_the_NET_style_position)
{
    EXPECT_EQ(parse_error_message("{\n  \"a\": x\n}"),
              "'x' is an invalid start of a value. LineNumber: 1 | BytePositionInLine: 7.");
    EXPECT_EQ(parse_error_message("[1 2]"),
              "Expected ',' or ']' after an array element, but found '2'. LineNumber: 0 | BytePositionInLine: 3.");
    EXPECT_EQ(parse_error_message("[1"),
              "Expected ',' or ']' after an array element, but found end of data. LineNumber: 0 | "
              "BytePositionInLine: 2.");
    EXPECT_EQ(parse_error_message("{'a':1}"), "Expected a property name starting with '\"', but found '''. "
                                              "LineNumber: 0 | BytePositionInLine: 1.");
    EXPECT_EQ(parse_error_message("\"a\x01\""),
              "'0x01' is invalid within a JSON string. The string should be correctly escaped. LineNumber: 0 | "
              "BytePositionInLine: 2.");
    EXPECT_EQ(parse_error_message("nul"),
              "Expected the JSON literal 'null', but found end of data. LineNumber: 0 | BytePositionInLine: 3.");
}

// ---------------------------------------------------------------------------
// Value accessors
// ---------------------------------------------------------------------------

TEST(JsonValue, Typed_getters_throw_TypeError_with_the_NET_message)
{
    // 05 CT-SRCH-04 [D]: ParseTimetable on the root "[1,2,3]".
    const json::Value array = json::parse("[1,2,3]");
    EXPECT_EQ(type_error_message([&] { (void)array.find("outboundFlights"); }),
              "The requested operation requires an element of type 'Object', but the target element has type "
              "'Array'.");

    const json::Value text = json::parse(R"("true")");
    const json::Value null = json::parse("null");
    const json::Value yes = json::parse("true");
    const json::Value no = json::parse("false");
    const json::Value number = json::parse("1");
    const json::Value object = json::parse("{}");
    const auto expect_message = [](std::string_view expected, std::string_view actual, std::string message) {
        EXPECT_EQ(message, std::format("The requested operation requires an element of type '{}', but the target "
                                       "element has type '{}'.",
                                       expected, actual));
    };
    expect_message("Boolean", "String", type_error_message([&] { (void)text.as_bool(); }));
    expect_message("String", "Null", type_error_message([&] { (void)null.as_string(); }));
    expect_message("Number", "True", type_error_message([&] { (void)yes.number_lexeme(); }));
    expect_message("Number", "False", type_error_message([&] { (void)no.as_double(); }));
    expect_message("Array", "Object", type_error_message([&] { (void)object.as_array(); }));
    expect_message("Array", "Number", type_error_message([&] { (void)number.at(0); }));
    expect_message("Object", "Number", type_error_message([&] { (void)number.as_object(); }));
    expect_message("Object", "String", type_error_message([&] { (void)text.get_string("a"); }));
    expect_message("Object", "Null", type_error_message([&] { (void)null.find("a"); }));
}

TEST(JsonValue, Numbers_keep_their_exact_source_lexeme)
{
    // 04 §9.4 / 03 §7.1: money is parsed from the text, never through double.
    for (const std::string_view lexeme :
         {"30790.0", "54.99", "1.5e2", "-0", "1e40", "129540.00", "0.0", "1E+2", "-12.50e-3", "4.1058", "1.0"}) {
        SCOPED_TRACE(lexeme);
        EXPECT_EQ(lexeme_of(lexeme), lexeme);
        const json::Value wrapped = json::parse(std::format(R"({{"amount":{}}})", lexeme));
        EXPECT_EQ(wrapped.find("amount")->number_lexeme(), lexeme);
    }
}

TEST(JsonValue, As_double_is_the_nearest_double)
{
    EXPECT_EQ(json::parse("54.99").as_double(), 54.99);
    EXPECT_EQ(json::parse("1.5e2").as_double(), 150.0);
    EXPECT_EQ(json::parse("30790.0").as_double(), 30790.0);
    EXPECT_EQ(json::parse("1e40").as_double(), 1e40);
    EXPECT_EQ(json::parse("47.0708678").as_double(), 47.0708678);
    EXPECT_EQ(json::parse("-81.2496068").as_double(), -81.2496068);
    EXPECT_EQ(json::parse("19.784722222222225").as_double(), 19.784722222222225);
    EXPECT_EQ(json::parse("1.7976931348623157e308").as_double(), std::numeric_limits<double>::max());
    EXPECT_EQ(json::parse("4.9e-324").as_double(), std::numeric_limits<double>::denorm_min());

    const double negative_zero = json::parse("-0").as_double();
    EXPECT_EQ(negative_zero, 0.0);
    EXPECT_TRUE(std::signbit(negative_zero));
}

TEST(JsonValue, As_double_rejects_overflow_and_flushes_underflow_to_zero)
{
    // JsonElement.GetDouble: an infinite result is a FormatException; underflow gives ±0.
    for (const std::string_view text : {"1e400", "-1e400", "1000e306", "1e99999999999999999999", "0.1e310"}) {
        SCOPED_TRACE(text);
        EXPECT_THROW((void)json::parse(text).as_double(), json::FormatError);
    }
    for (const std::string_view text : {"1e-400", "0.001e-322", "123456789e-400", "1e-99999999999999999999"}) {
        SCOPED_TRACE(text);
        const double value = json::parse(text).as_double();
        EXPECT_EQ(value, 0.0);
        EXPECT_FALSE(std::signbit(value));
    }
    const double negative = json::parse("-1e-400").as_double();
    EXPECT_EQ(negative, 0.0);
    EXPECT_TRUE(std::signbit(negative));
}

TEST(JsonValue, As_double_and_number_ignore_the_C_locale)
{
    // 02 §5.1: parse numbers locale-independently.
    const std::string previous = std::setlocale(LC_ALL, nullptr);
    ASSERT_NE(std::setlocale(LC_ALL, "de-DE"), nullptr);
    const double parsed = json::parse("47.0708678").as_double();
    const std::string written = json::Writer{}.number(47.0708678).take();
    std::setlocale(LC_ALL, previous.c_str());

    EXPECT_EQ(parsed, 47.0708678);
    EXPECT_EQ(written, "47.0708678");
}

TEST(JsonValue, Try_int32_follows_System_Text_Json_Int32_rules)
{
    const struct {
        std::string_view text;
        std::optional<std::int32_t> expected;
    } cases[] = {
        {"250", 250},
        {"0", 0},
        {"-0", 0},
        {"-250", -250},
        {"2147483647", std::numeric_limits<std::int32_t>::max()},
        {"-2147483648", std::numeric_limits<std::int32_t>::min()},
        {"2147483648", std::nullopt},
        {"-2147483649", std::nullopt},
        {"99999999999999999999", std::nullopt},
        {"250.5", std::nullopt},  // 01 §13: a fractional int is a JsonException
        {"250.0", std::nullopt},
        {"2.5e2", std::nullopt},
        {"1e2", std::nullopt},
        {"1E2", std::nullopt},
        {"0e0", std::nullopt},
        {R"("250")", std::nullopt},  // 01 §13: "Adults": "two" style type mismatch
        {"null", std::nullopt},
        {"true", std::nullopt},
        {"[250]", std::nullopt},
        {R"({"a":250})", std::nullopt},
    };
    for (const auto& [text, expected] : cases) {
        SCOPED_TRACE(text);
        EXPECT_EQ(json::parse(text).try_int32(), expected);
    }
}

TEST(JsonValue, Arrays_keep_source_order_and_check_bounds)
{
    const json::Value root = json::parse(R"([1, "a", null, [true], {}])");
    const auto items = root.as_array();
    ASSERT_EQ(items.size(), 5u);
    std::vector<json::Kind> kinds;
    for (const json::Value& item : items) {
        kinds.push_back(item.kind());
    }
    EXPECT_EQ(kinds, (std::vector{json::Kind::number, json::Kind::string, json::Kind::null, json::Kind::array,
                                  json::Kind::object}));
    EXPECT_EQ(root.at(1).as_string(), "a");
    EXPECT_TRUE(root.at(3).at(0).as_bool());
    EXPECT_THROW((void)root.at(5), std::out_of_range);
    const json::Value empty = json::parse("[]");
    EXPECT_TRUE(empty.as_array().empty());
}

TEST(JsonValue, Objects_keep_every_member_in_source_order)
{
    const json::Value root = json::parse(R"({"b":1,"a":2,"b":3})");
    const auto members = root.as_object();
    ASSERT_EQ(members.size(), 3u);
    EXPECT_EQ(members[0].name(), "b");
    EXPECT_EQ(members[0].value().number_lexeme(), "1");
    EXPECT_EQ(members[1].name(), "a");
    EXPECT_EQ(members[2].name(), "b");
    EXPECT_EQ(members[2].value().number_lexeme(), "3");
}

TEST(JsonValue, Find_is_ordinal_case_sensitive_and_the_last_duplicate_wins)
{
    // 04 §9.4 (verified on .NET): ordinal, case-sensitive; duplicates → last value.
    const json::Value root = json::parse(R"({"b":1,"a":2,"b":3,"x":{"y":1},"x":[2],"":5})");
    EXPECT_EQ(root.find("b")->number_lexeme(), "3");
    EXPECT_EQ(root.find("a")->number_lexeme(), "2");
    EXPECT_TRUE(root.find("x")->is_array());
    EXPECT_EQ(root.find("")->number_lexeme(), "5");
    EXPECT_EQ(root.find("B"), nullptr);
    EXPECT_EQ(root.find("c"), nullptr);
    EXPECT_EQ(root.find("b "), nullptr);
    const json::Value empty = json::parse("{}");
    EXPECT_EQ(empty.find("a"), nullptr);
}

TEST(JsonValue, Find_compares_decoded_names_without_normalisation)
{
    const json::Value escaped = json::parse(R"({"\u0041":1,"a":2})");
    EXPECT_EQ(escaped.find("A")->number_lexeme(), "1");
    EXPECT_EQ(escaped.find("a")->number_lexeme(), "2");

    const json::Value decomposed = json::parse(R"({"e\u0301":1})");
    EXPECT_EQ(decomposed.find("\xC3\xA9"), nullptr);  // precomposed é is a different name
    EXPECT_NE(decomposed.find("e\xCC\x81"), nullptr);
}

TEST(JsonValue, Get_string_returns_only_JSON_strings)
{
    // Str(e, n) of 02 §5.1: a value only when the member exists and its kind is String.
    const json::Value root = json::parse(R"({"iata":"BUD","num":5,"nul":null,"bool":true,"arr":["x"],"obj":{},)"
                                         R"("empty":"","dup":5,"dup":"last","dup2":"first","dup2":7})");
    EXPECT_EQ(root.get_string("iata"), "BUD");
    EXPECT_EQ(root.get_string("empty"), "");
    EXPECT_EQ(root.get_string("dup"), "last");
    for (const std::string_view name : {"num", "nul", "bool", "arr", "obj", "dup2", "missing", "IATA"}) {
        SCOPED_TRACE(name);
        EXPECT_EQ(root.get_string(name), std::nullopt);
    }
}

TEST(JsonValue, Copies_are_deep_and_moves_are_cheap)
{
    json::Value original = json::parse(R"({"a":[1,2,{"b":"c"}]})");
    const json::Value copy = original;
    const json::Value moved = std::move(original);
    EXPECT_EQ(copy.find("a")->at(2).get_string("b"), "c");
    EXPECT_EQ(moved.find("a")->at(2).get_string("b"), "c");
    EXPECT_NE(&copy.find("a")->at(0), &moved.find("a")->at(0));

    json::Value assigned;
    assigned = moved;
    EXPECT_EQ(assigned.find("a")->as_array().size(), 3u);
}

// ---------------------------------------------------------------------------
// Fixtures (05 §2.2)
// ---------------------------------------------------------------------------

TEST(JsonFixtures, Every_fixture_file_parses)
{
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{fixtures_dir()}) {
        if (entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    EXPECT_EQ(files.size(), 11u);
    for (const auto& path : files) {
        SCOPED_TRACE(path.filename().string());
        EXPECT_NO_THROW((void)json::parse(read_file(path)));
    }
}

TEST(JsonFixtures, Timetable_amounts_keep_their_lexemes)
{
    const json::Value ltn = parse_fixture("timetable_v2_bud_ltn.json");
    const auto rows = ltn.find("outboundFlights")->as_array();
    ASSERT_EQ(rows.size(), 11u);
    EXPECT_EQ(rows[0].get_string("departureStation"), "BUD");
    EXPECT_EQ(rows[0].find("price")->find("amount")->number_lexeme(), "30790.0");
    EXPECT_EQ(rows[0].find("price")->get_string("currencyCode"), "HUF");
    EXPECT_TRUE(rows[0].find("price")->find("exchangedAmount")->is_null());
    EXPECT_TRUE(rows[0].find("departureDates")->at(0).find("isCheapestOfTheDay")->is_true());
    EXPECT_TRUE(ltn.find("returnFlights")->is_null());

    const json::Value check = parse_fixture("timetable_checkprice.json");
    const auto check_rows = check.find("outboundFlights")->as_array();
    ASSERT_EQ(check_rows.size(), 4u);
    EXPECT_EQ(check_rows[0].get_string("priceType"), "checkPrice");
    EXPECT_EQ(check_rows[0].find("price")->find("amount")->number_lexeme(), "0.0");
    EXPECT_EQ(check_rows[0].find("originalPrice")->find("amount")->number_lexeme(), "129540.00");

    const json::Value nap = parse_fixture("timetable_v2_bud_nap.json");
    EXPECT_EQ(nap.find("outboundFlights")->as_array().size(), 10u);
}

TEST(JsonFixtures, Map_subset_decodes_names_and_keeps_kinds)
{
    const json::Value map = parse_fixture("map_subset.json");
    const auto cities = map.find("cities")->as_array();
    ASSERT_EQ(cities.size(), 12u);
    EXPECT_TRUE(map.find("javascript")->is_null());

    const json::Value* krk = nullptr;
    int fake_stations = 0;
    for (const json::Value& city : cities) {
        if (city.get_string("iata") == "KRK") {
            krk = &city;
        }
        const json::Value* fake = city.find("isFakeStation");
        fake_stations += fake != nullptr && fake->is_true() ? 1 : 0;
    }
    EXPECT_EQ(fake_stations, 1);
    ASSERT_NE(krk, nullptr);
    EXPECT_EQ(krk->get_string("shortName"), "Krak\xC3\xB3w\r\n");
    EXPECT_EQ(krk->get_string("mac"), "SPQ");
    EXPECT_EQ(krk->get_string("currencyCode"), "PLN");
    EXPECT_EQ(krk->find("latitude")->number_lexeme(), "50.07777777777778");
    EXPECT_EQ(krk->find("longitude")->as_double(), 19.784722222222225);
}

TEST(JsonFixtures, Fx_sources_keep_rate_lexemes)
{
    const json::Value frankfurter = parse_fixture("fx_frankfurter_v2.json");
    const auto rows = frankfurter.as_array();
    ASSERT_EQ(rows.size(), 170u);
    EXPECT_EQ(rows[0].find("rate")->number_lexeme(), "4.1058");
    const json::Value* eur = nullptr;
    for (const json::Value& row : rows) {
        if (row.get_string("quote") == "EUR") {
            eur = row.find("rate");
        }
    }
    ASSERT_NE(eur, nullptr);
    EXPECT_EQ(eur->number_lexeme(), "1.0");

    const json::Value fawaz = parse_fixture("fx_fawaz.json");  // tab-indented, LF line breaks
    EXPECT_EQ(fawaz.get_string("date"), "2026-11-01");
    const json::Value& rates = *fawaz.find("eur");
    EXPECT_EQ(rates.as_object().size(), 340u);
    EXPECT_EQ(rates.find("huf")->number_lexeme(), "367.41835572");
    EXPECT_EQ(rates.find("gbp")->number_lexeme(), "0.83637627");
    EXPECT_EQ(rates.find("HUF"), nullptr);
}

TEST(JsonFixtures, Validation_codes_and_geocoder_answers)
{
    const json::Value market = parse_fixture("error_invalid_market.json");
    ASSERT_EQ(market.find("validationCodes")->as_array().size(), 1u);
    EXPECT_EQ(market.find("validationCodes")->at(0).as_string(), "InvalidMarket");
    const json::Value range = parse_fixture("error_invalid_range.json");
    EXPECT_EQ(range.find("validationCodes")->at(0).as_string(), "InvalidTimeDateRange");

    const json::Value london = parse_fixture("geo_photon_london.json");
    const auto features = london.find("features")->as_array();
    ASSERT_EQ(features.size(), 3u);
    const json::Value& coordinates = *features[0].find("geometry")->find("coordinates");  // [lon, lat]
    EXPECT_EQ(coordinates.at(0).as_double(), -0.1277653);
    EXPECT_EQ(coordinates.at(1).as_double(), 51.5074456);

    const json::Value nominatim = parse_fixture("geo_nominatim_graz.json");
    ASSERT_EQ(nominatim.as_array().size(), 1u);
    EXPECT_EQ(nominatim.at(0).get_string("lat"), "47.0708678");  // a string, not a number
    EXPECT_TRUE(nominatim.at(0).get_string("licence")->starts_with("Data \xC2\xA9 OpenStreetMap"));
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

std::string timetable_body(std::string_view from, std::string_view to, std::string_view start, std::string_view end,
                           std::string_view price_type, int adults)
{
    json::Writer writer;
    writer.begin_object()
        .key("flightList")
        .begin_array()
        .begin_object()
        .key("departureStation")
        .string(from)
        .key("arrivalStation")
        .string(to)
        .key("from")
        .string(start)
        .key("to")
        .string(end)
        .end_object()
        .end_array()
        .key("priceType")
        .string(price_type)
        .key("adultCount")
        .integer(adults)
        .key("childCount")
        .integer(0)
        .key("infantCount")
        .integer(0)
        .end_object();
    return writer.take();
}

TEST(JsonWriter, Compact_timetable_body_is_byte_exact)
{
    // 04 §9.3 test Timetable_body_matches_the_site_format.
    EXPECT_EQ(timetable_body("BUD", "LTN", "2026-11-10", "2026-12-21", "wdc", 2),
              R"({"flightList":[{"departureStation":"BUD","arrivalStation":"LTN","from":"2026-11-10","to":"2026-12-21"}],)"
              R"("priceType":"wdc","adultCount":2,"childCount":0,"infantCount":0})");
    // 05 CT-API-01 request body.
    EXPECT_EQ(timetable_body("BUD", "LTN", "2026-11-10", "2026-11-20", "regular", 1),
              R"({"flightList":[{"departureStation":"BUD","arrivalStation":"LTN","from":"2026-11-10","to":"2026-11-20"}],)"
              R"("priceType":"regular","adultCount":1,"childCount":0,"infantCount":0})");
}

TEST(JsonWriter, Escapes_like_JavaScriptEncoder_Default)
{
    const struct {
        std::string_view text;
        std::string_view json;
    } cases[] = {
        {"B\xC3\x9C<D", R"("B\u00DC\u003CD")"},  // 04 §9.3: "bü<d" upper-cased
        {"L\"TN", R"("L\u0022TN")"},             // 04 §9.3
        {"\xE2\x86\x92", R"("\u2192")"},         // → (01 §17.3)
        {">", R"("\u003E")"},
        {"'", R"("\u0027")"},
        {"+", R"("\u002B")"},
        {"\xE2\x80\x9C", R"("\u201C")"},         // “
        {"&", R"("\u0026")"},
        {"<", R"("\u003C")"},
        {"`", R"("\u0060")"},
        {"Krak\xC3\xB3w", R"("Krak\u00F3w")"},   // 02 §6.9
        {"\xE2\x80\x93", R"("\u2013")"},         // 02 §6.9
        {"\"", R"("\u0022")"},                   // 02 §6.9
        {"\xC3\xA9\xC2\xA0", R"("\u00E9\u00A0")"},
        {"\\", R"("\\")"},
        {"/", R"("/")"},
        {"\b\t\n\f\r", R"("\b\t\n\f\r")"},
        {"\x01\x0B\x1F\x7F", R"("\u0001\u000B\u001F\u007F")"},
        {std::string_view{"a\0b", 3}, R"("a\u0000b")"},
        {"\xF0\x9F\x98\x80", R"("\uD83D\uDE00")"},  // beyond the BMP: surrogate pair
        {"\xF4\x8F\xBF\xBF", R"("\uDBFF\uDFFF")"},
        {"Budapest - London: 3 nights, 100% ok? (yes) [1] {2} ~!@#$^*_=|;:,./",
         R"("Budapest - London: 3 nights, 100% ok? (yes) [1] {2} ~!@#$^*_=|;:,./")"},
        {"", R"("")"},
    };
    for (const auto& [text, expected] : cases) {
        SCOPED_TRACE(testing::PrintToString(std::string{text}));
        EXPECT_EQ(quoted(text), expected);
    }
}

TEST(JsonWriter, Ill_formed_UTF8_becomes_U_FFFD_per_maximal_subpart)
{
    EXPECT_EQ(quoted("\xFF"), R"("\uFFFD")");
    EXPECT_EQ(quoted("a\xE2\x82" "b"), R"("a\uFFFDb")");
    EXPECT_EQ(quoted("\xC0\xAF"), R"("\uFFFD\uFFFD")");
    EXPECT_EQ(quoted("\xED\xA0\x80"), R"("\uFFFD\uFFFD\uFFFD")");
    EXPECT_EQ(quoted("\xF0\x9F\x98"), R"("\uFFFD")");
}

TEST(JsonWriter, Keys_are_escaped_like_strings)
{
    EXPECT_EQ(json::Writer{}.begin_object().key("a<b").integer(1).key("Krak\xC3\xB3w").null().end_object().take(),
              R"({"a\u003Cb":1,"Krak\u00F3w":null})");
}

TEST(JsonWriter, Written_strings_parse_back_unchanged)
{
    for (const std::string_view text : {"B\xC3\x9C<D & 'x' + `y` \"z\""sv, "Krak\xC3\xB3w\r\n"sv,
                                        "\xE2\x80\x93\xE2\x86\x92"sv, "\xF0\x9F\x98\x80 \x01\x7F\\/"sv, "\0"sv}) {
        SCOPED_TRACE(testing::PrintToString(std::string{text}));
        EXPECT_EQ(decoded(quoted(text)), text);
    }
}

TEST(JsonWriter, Indented_settings_match_the_01_13_layout)
{
    json::Writer writer{json::Layout::indented};
    writer.begin_object()
        .key("From").string("Budapest")
        .key("To").string("London")
        .key("FromRadiusKm").integer(250)
        .key("ToRadiusKm").integer(100)
        .key("Adults").integer(1)
        .key("DiscountClub").boolean(false)
        .key("Currency").string("EUR")
        .key("MinNights").integer(2)
        .key("MaxNights").integer(10)
        .key("WindowWidth").integer(1320)
        .key("WindowHeight").integer(820)
        .key("Maximized").boolean(false)
        .end_object();
    EXPECT_EQ(writer.str(), crlf_lines({
                                R"({)",
                                R"(  "From": "Budapest",)",
                                R"(  "To": "London",)",
                                R"(  "FromRadiusKm": 250,)",
                                R"(  "ToRadiusKm": 100,)",
                                R"(  "Adults": 1,)",
                                R"(  "DiscountClub": false,)",
                                R"(  "Currency": "EUR",)",
                                R"(  "MinNights": 2,)",
                                R"(  "MaxNights": 10,)",
                                R"(  "WindowWidth": 1320,)",
                                R"(  "WindowHeight": 820,)",
                                R"(  "Maximized": false)",
                                R"(})",
                            }));
}

TEST(JsonWriter, Indented_nesting_keeps_empty_containers_inline)
{
    json::Writer writer{json::Layout::indented};
    writer.begin_object()
        .key("a").begin_array()
            .integer(1)
            .begin_object().key("b").null().end_object()
            .begin_array().end_array()
        .end_array()
        .key("c").begin_object().end_object()
        .key("d").string("x\xE2\x86\x92")
        .end_object();
    EXPECT_EQ(writer.str(), crlf_lines({
                                R"({)",
                                R"(  "a": [)",
                                R"(    1,)",
                                R"(    {)",
                                R"(      "b": null)",
                                R"(    },)",
                                R"(    [])",
                                R"(  ],)",
                                R"(  "c": {},)",
                                R"(  "d": "x\u2192")",
                                R"(})",
                            }));
    EXPECT_EQ(json::Writer{json::Layout::indented}.begin_array().end_array().take(), "[]");
    EXPECT_EQ(json::Writer{json::Layout::indented}.integer(5).take(), "5");
}

TEST(JsonWriter, Compact_nesting_has_no_white_space)
{
    json::Writer writer;
    writer.begin_array()
        .begin_array().end_array()
        .begin_array().integer(1).begin_array().integer(2).end_array().end_array()
        .begin_object().end_object()
        .boolean(true)
        .boolean(false)
        .null()
        .end_array();
    EXPECT_EQ(writer.str(), "[[],[1,[2]],{},true,false,null]");
}

TEST(JsonWriter, Doubles_use_the_shortest_round_trip_form)
{
    const struct {
        double value;
        std::string_view text;
    } cases[] = {
        {47.0708678, "47.0708678"},
        {-81.2496068, "-81.2496068"},
        {0.1, "0.1"},
        {12.3, "12.3"},
        {250.0, "250"},
        {-0.0, "-0"},
        {1e21, "1e+21"},
        {1.5e-7, "1.5e-07"},
        {std::numeric_limits<double>::denorm_min(), "5e-324"},
        {std::numeric_limits<double>::max(), "1.7976931348623157e+308"},
    };
    for (const auto& [value, text] : cases) {
        SCOPED_TRACE(text);
        const std::string written = json::Writer{}.number(value).take();
        EXPECT_EQ(written, text);
        EXPECT_EQ(json::parse(written).as_double(), value);
    }
    EXPECT_THROW((void)json::Writer{}.number(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    EXPECT_THROW((void)json::Writer{}.number(std::numeric_limits<double>::infinity()), std::invalid_argument);
    EXPECT_THROW((void)json::Writer{}.number(-std::numeric_limits<double>::infinity()), std::invalid_argument);
}

TEST(JsonWriter, Integers_cover_the_int64_range)
{
    EXPECT_EQ(json::Writer{}.integer(0).take(), "0");
    EXPECT_EQ(json::Writer{}.integer(-1).take(), "-1");
    EXPECT_EQ(json::Writer{}.integer(std::numeric_limits<std::int64_t>::max()).take(), "9223372036854775807");
    EXPECT_EQ(json::Writer{}.integer(std::numeric_limits<std::int64_t>::min()).take(), "-9223372036854775808");
}

TEST(JsonWriter, Number_lexemes_are_written_verbatim_and_validated)
{
    // 03 §7.2 disk cache shape; decimal text keeps its scale ("EUR": 1.0).
    EXPECT_EQ(json::Writer{}
                  .begin_object()
                  .key("Date").string("2026-11-02")
                  .key("Source").string("Frankfurter")
                  .key("PerEur").begin_object()
                      .key("AED").number_lexeme("4.1061")
                      .key("EUR").number_lexeme("1.0")
                      .key("HUF").number_lexeme("366.3")
                  .end_object()
                  .end_object()
                  .take(),
              R"({"Date":"2026-11-02","Source":"Frankfurter","PerEur":{"AED":4.1061,"EUR":1.0,"HUF":366.3}})");
    for (const std::string_view lexeme : {"30790.0", "-0", "1.5e2", "1E+2", "0"}) {
        EXPECT_EQ(json::Writer{}.number_lexeme(lexeme).take(), lexeme);
    }
    for (const std::string_view lexeme : {"", "01", "+1", "1.", ".5", "NaN", "1e", "-", " 1", "1 ", "0x10", "1,5", "1e5.5"}) {
        SCOPED_TRACE(lexeme);
        EXPECT_THROW((void)json::Writer{}.number_lexeme(lexeme), std::invalid_argument);
    }
}

TEST(JsonWriter, Misuse_is_a_logic_error)
{
    {
        json::Writer writer;
        writer.begin_object();
        EXPECT_THROW(writer.string("value without key"), std::logic_error);
        writer.key("a");
        EXPECT_THROW(writer.key("b"), std::logic_error);
        EXPECT_THROW(writer.end_object(), std::logic_error);
        EXPECT_THROW(writer.end_array(), std::logic_error);
    }
    {
        json::Writer writer;
        EXPECT_THROW(writer.key("a"), std::logic_error);
        EXPECT_THROW(writer.end_object(), std::logic_error);
        EXPECT_THROW(writer.end_array(), std::logic_error);
        writer.begin_array();
        EXPECT_THROW(writer.key("a"), std::logic_error);
        EXPECT_THROW(writer.end_object(), std::logic_error);
        EXPECT_FALSE(writer.complete());
        EXPECT_THROW((void)writer.str(), std::logic_error);
        EXPECT_THROW((void)writer.take(), std::logic_error);
        writer.end_array();
        EXPECT_TRUE(writer.complete());
        EXPECT_THROW(writer.integer(2), std::logic_error);
        EXPECT_THROW(writer.begin_object(), std::logic_error);
    }
    EXPECT_THROW((void)json::Writer{}.str(), std::logic_error);
}

TEST(JsonWriter, Take_moves_the_document_out_and_resets)
{
    json::Writer writer;
    writer.string("first");
    EXPECT_EQ(writer.take(), R"("first")");
    EXPECT_FALSE(writer.complete());
    writer.boolean(true);
    EXPECT_EQ(writer.take(), "true");
}

} // namespace
