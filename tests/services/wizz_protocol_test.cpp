// Wizz protocol text: 05 §3.3 CT-APIP-01..08 (with the inline B1/V1 bodies of
// 05 §2.7 and the fixture rows of 05 §2.4), P-29 (body part), P-30 (parse part),
// P-35, P-06 (snippet part), the verified edge-case table of 04 §7.3, the
// System.Version rules of 04 §7.3, D-15 (04 §7.4), the date-parse table and the
// malformed shapes of 04 §9.4, 04 §9.3 bodies and the 04 §11.5 helpers.
// Expected values beyond the spec tables were recorded from .NET 10 (Regex,
// Version.TryParse, JsonElement.GetDecimal) and are marked "(.NET)".

#include "wizz/services/wizz_protocol.hpp"

#include "wizz/core/decimal.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/text.hpp"
#include "wizz/core/time.hpp"

#include "support/fixtures.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using wizz::Date;
using wizz::DayFare;
using wizz::Decimal;
using wizz::FareStatus;
using wizz::LocalDateTime;
using wizz::Money;
using wizz::PriceType;
using wizz::protocol::build_timetable_body;
using wizz::protocol::extract_api_url;
using wizz::protocol::is_valid_cached_api_url;
using wizz::protocol::looks_like_json;
using wizz::protocol::parse_timetable;
using wizz::protocol::parse_validation_codes;
using wizz::protocol::snippet;
using wizz::protocol::SystemVersion;

using namespace std::string_literals;
using namespace std::string_view_literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

[[nodiscard]] LocalDateTime at(int year, unsigned month, unsigned day, int hour, int minute, int second = 0)
{
    return wizz::make_local(Date::from_ymd(year, month, day), hour, minute, second);
}

[[nodiscard]] Decimal dec(std::string_view text)
{
    const std::optional<Decimal> value = Decimal::parse(text);
    if (!value) {
        throw std::invalid_argument("bad decimal literal in test");
    }
    return *value;
}

[[nodiscard]] std::optional<std::string> url(std::string_view html)
{
    return extract_api_url(html);
}

constexpr std::string_view kTypeErrorArray =
    "The requested operation requires an element of type 'Object', but the target element has type 'Array'.";

/// A one-row timetable around `row` (a JSON object text).
[[nodiscard]] std::string timetable(std::string_view row)
{
    return R"({"outboundFlights":[)"s + std::string{row} + R"(],"returnFlights":null})";
}

/// A minimal valid V2 row with the given departureDate text (JSON-escaped by the writer).
[[nodiscard]] std::string row_with_departure(std::string_view departure_date, std::string_view dates_json = "[]")
{
    wizz::json::Writer writer;
    writer.begin_object()
        .key("departureStation")
        .string("BUD")
        .key("arrivalStation")
        .string("LTN")
        .key("departureDate")
        .string(departure_date)
        .key("price")
        .begin_object()
        .key("amount")
        .number_lexeme("19290.0")
        .key("currencyCode")
        .string("HUF")
        .end_object()
        .key("priceType")
        .string("price")
        .end_object();
    std::string row = writer.take();
    row.insert(row.size() - 1, R"(,"departureDates":)"s + std::string{dates_json});
    return row;
}

/// One expected fixture row (05 §2.4).
struct ExpectedRow {
    std::string_view from;
    std::string_view to;
    unsigned month;
    unsigned day;
    int hour;
    int minute;
    std::string_view amount;
    std::vector<std::pair<int, int>> departures;
    bool mac;
    FareStatus status = FareStatus::available;
};

void expect_rows(const std::vector<DayFare>& fares, const std::vector<ExpectedRow>& expected)
{
    ASSERT_EQ(fares.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE("row " + std::to_string(i + 1));
        const DayFare& fare = fares[i];
        const ExpectedRow& row = expected[i];
        const Date date = Date::from_ymd(2026, row.month, row.day);
        EXPECT_EQ(fare.from, row.from);
        EXPECT_EQ(fare.to, row.to);
        EXPECT_EQ(fare.date, date);
        EXPECT_EQ(fare.cheapest_departure, wizz::make_local(date, row.hour, row.minute));
        EXPECT_EQ(fare.price_per_person, (Money{dec(row.amount), "HUF"}));
        EXPECT_EQ(fare.price_per_person.amount.to_string(), row.amount);  // scale kept ("30790.0")
        EXPECT_EQ(fare.status, row.status);
        EXPECT_EQ(fare.has_mac_flight, row.mac);
        std::vector<LocalDateTime> departures;
        for (const auto& [hour, minute] : row.departures) {
            departures.push_back(wizz::make_local(date, hour, minute));
        }
        EXPECT_EQ(fare.departures, departures);
    }
}

// ---------------------------------------------------------------------------
// ExtractApiUrl (04 §7.3, protocol-reference §3)
// ---------------------------------------------------------------------------

TEST(WizzProtocol, CT_APIP_01_ExtractApiUrl_reads_nuxt_config)
{
    EXPECT_EQ(url(wizz::test::kHomeHtml), "https://be.wizzair.com/29.23.1/Api");
    EXPECT_EQ(url(wizz::test::home_html_for("30.0.1")), "https://be.wizzair.com/30.0.1/Api");
}

TEST(WizzProtocol, CT_APIP_02_ExtractApiUrl_handles_json_escaped_and_fallback_forms)
{
    // [A] JSON-escaped: literal backslash-slash pairs are unescaped.
    EXPECT_EQ(url(R"({"apiUrl":"https:\/\/be.wizzair.com\/30.1.0\/Api"})"), "https://be.wizzair.com/30.1.0/Api");
    // [A] Fallback: the highest version compared numerically (a string comparison would pick 29.9.2).
    EXPECT_EQ(url("src=https://be.wizzair.com/29.9.2/Api/x and https://be.wizzair.com/29.10.0/Api/y"),
              "https://be.wizzair.com/29.10.0/Api");
    // [A]
    EXPECT_EQ(url("<html>nothing here</html>"), std::nullopt);
    // [D] A 5-part version is ignored by System.Version.
    EXPECT_EQ(url("https://be.wizzair.com/1.2.3.4.5/Api"), std::nullopt);
    // [D] Version(29,10) < Version(29,10,0).
    EXPECT_EQ(url("be.wizzair.com/29.10/Api be.wizzair.com/29.10.0/Api"), "https://be.wizzair.com/29.10.0/Api");
}

TEST(WizzProtocol, P_35_ExtractApiUrl_corner_cases)
{
    EXPECT_EQ(url("https://be.wizzair.com/1.2.3.4.5/Api"), std::nullopt);
    EXPECT_EQ(url("be.wizzair.com/29.10/Api be.wizzair.com/29.10.0/Api"), "https://be.wizzair.com/29.10.0/Api");
}

/// Every row of the 04 §7.3 "Verified edge cases" table. A decoy "be.wizzair.com/999.0/Api"
/// after a primary-path input proves the primary regex (not the fallback) produced the answer.
TEST(WizzProtocol, ExtractApiUrl_verified_edge_case_table_04_7_3)
{
    constexpr std::string_view kDecoy = " be.wizzair.com/999.0/Api";
    const auto primary = [&](std::string_view fragment) { return url(std::string{fragment} + std::string{kDecoy}); };
    const std::string expected = "https://be.wizzair.com/29.23.1/Api";

    // Primary path.
    EXPECT_EQ(primary(R"(apiUrl:"https://be.wizzair.com/29.23.1/Api")"), expected);              // Nuxt form
    EXPECT_EQ(primary(R"("apiUrl" : "https://be.wizzair.com/29.23.1/Api")"), expected);          // quoted key
    EXPECT_EQ(primary(R"(apiUrl\":\"https:\/\/be.wizzair.com\/29.23.1\/Api\")"), expected);      // JSON in a JS string
    EXPECT_EQ(primary(R"(apiUrl:"https:\/\/be.wizzair.com/29.23.1\/Api")"), expected);           // mixed escaping
    EXPECT_EQ(primary(R"(apiUrl:"https://be.wizzair.com/29.23.1/ApiX")"), expected);             // no boundary
    EXPECT_EQ(primary(R"(apiUrl:"https://be.wizzair.com/29.23.1/Api/")"), expected);
    EXPECT_EQ(url(R"(x apiUrl:"https://be.wizzair.com/28.0.0/Api" y be.wizzair.com/30.0.0/Api)"),
              "https://be.wizzair.com/28.0.0/Api");  // the primary wins over a higher version elsewhere

    // Fallback path.
    EXPECT_EQ(url("apiUrl:'https://be.wizzair.com/29.23.1/Api'"), expected);    // single quotes
    EXPECT_EQ(url(R"(APIURL:"https://be.wizzair.com/29.23.1/Api")"), expected);  // key case differs
    EXPECT_EQ(url(R"(apiUrl:"http://be.wizzair.com/29.23.1/Api")"), expected);   // fallback always builds https
    EXPECT_EQ(url(R"(apiUrl:"https://be.wizzair.com/29/Api" and be.wizzair.com/29.5.0/Api)"),
              "https://be.wizzair.com/29.5.0/Api");  // one component is not enough
    EXPECT_EQ(url("be.wizzair.com/29.9.2/Api/x \xE2\x80\xA6 be.wizzair.com/29.10.0/Api/y"),
              "https://be.wizzair.com/29.10.0/Api");  // numeric comparison
    EXPECT_EQ(url("be.wizzair.com/29.10/Api be.wizzair.com/29.10.0/Api"), "https://be.wizzair.com/29.10.0/Api");
    EXPECT_EQ(url("be.wizzair.com/30.1.2.3.4/Api be.wizzair.com/29.1.0/Api"),
              "https://be.wizzair.com/29.1.0/Api");  // the 5-part version is dropped
    EXPECT_EQ(url("be.wizzair.com/29.01.0/Api be.wizzair.com/29.1.0/Api"),
              "https://be.wizzair.com/29.01.0/Api");  // tie: first wins, text kept

    // No match at all.
    EXPECT_EQ(url(R"(apiUrl:"https://be.wizzair.com/29.23.1/api")"), std::nullopt);  // lowercase api
    EXPECT_EQ(url("<html>nothing here</html>"), std::nullopt);
}

/// Regex details of 04 §7.3: `\s` is char.IsWhiteSpace, `\d` a BMP Nd digit, one optional
/// backslash per position, first primary match wins, the primary does not validate versions. (.NET)
TEST(WizzProtocol, ExtractApiUrl_mirrors_dotnet_regex_details)
{
    constexpr std::string_view kDecoy = " be.wizzair.com/999.0/Api";
    const auto with_decoy = [&](std::string_view fragment) {
        return url(std::string{fragment} + std::string{kDecoy});
    };
    const std::string expected = "https://be.wizzair.com/29.23.1/Api";
    const std::string fallback = "https://be.wizzair.com/999.0/Api";

    // Unicode white space around ':' (U+00A0, U+3000, U+0085, LF) is \s; U+200B is not.
    EXPECT_EQ(with_decoy("apiUrl\u00A0:\u3000\"https://be.wizzair.com/29.23.1/Api\""), expected);
    EXPECT_EQ(with_decoy("apiUrl\u0085:\n\"https://be.wizzair.com/29.23.1/Api\""), expected);
    EXPECT_EQ(with_decoy(R"(apiUrl : "https://be.wizzair.com/29.23.1/Api")"), expected);
    EXPECT_EQ(with_decoy(R"(apiUrl"  :  "https://be.wizzair.com/29.23.1/Api")"), expected);
    EXPECT_EQ(with_decoy("apiUrl\u200B:\"https://be.wizzair.com/29.23.1/Api\""), fallback);

    // One optional backslash before the key's quote, the value's quote and each '/'.
    EXPECT_EQ(with_decoy(R"(apiUrl\:"https://be.wizzair.com/29.23.1/Api")"), expected);
    EXPECT_EQ(with_decoy(R"(apiUrl:\"https://be.wizzair.com/29.23.1/Api")"), expected);
    EXPECT_EQ(with_decoy(R"(apiUrl:"https:\//be.wizzair.com\/29.23.1/Api")"), expected);
    EXPECT_EQ(with_decoy(R"(apiUrl:"https:\\/\\/be.wizzair.com/1.2/Api")"), fallback);
    EXPECT_EQ(with_decoy(R"(apiUrl\\":"https://be.wizzair.com/1.2/Api")"), fallback);
    EXPECT_EQ(with_decoy(R"(apiUrl"\:"https://be.wizzair.com/1.2/Api")"), fallback);
    EXPECT_EQ(with_decoy(R"(apiUrl\ :\ "https://be.wizzair.com/29.23.1/Api")"), fallback);
    EXPECT_EQ(with_decoy(R"(apiUrl:"https://be.wizzair.com/29.23.1\\/Api")"), fallback);

    // Exact shape: two slashes, a quote right before https, no extra key characters.
    EXPECT_EQ(with_decoy(R"(apiUrl:"https:/be.wizzair.com/29.23.1/Api")"), fallback);
    EXPECT_EQ(with_decoy(R"(apiUrl:"https:///be.wizzair.com/29.23.1/Api")"), fallback);
    EXPECT_EQ(with_decoy(R"(apiUrl:" https://be.wizzair.com/29.23.1/Api")"), fallback);
    EXPECT_EQ(with_decoy(R"(apiUrl""https://be.wizzair.com/29.23.1/Api")"), fallback);
    EXPECT_EQ(with_decoy(R"(apiUrlX:"https://be.wizzair.com/1.2/Api")"), fallback);
    EXPECT_EQ(with_decoy(R"(apiUrl:"https://be.wizzair.com/29.23.1.\/Api")"), fallback);

    // A failed "apiUrl" occurrence does not stop the scan; the first successful one wins.
    EXPECT_EQ(url(R"(apiUrl:"bad" apiUrl:"https://be.wizzair.com/7.8/Api" apiUrl:"https://be.wizzair.com/9.0/Api")"),
              "https://be.wizzair.com/7.8/Api");
    // The primary does not validate the version (the fallback would drop 5 parts).
    EXPECT_EQ(url(R"(apiUrl:"https://be.wizzair.com/1.2.3.4.5/Api")"), "https://be.wizzair.com/1.2.3.4.5/Api");

    // \d is any BMP decimal digit: Arabic-Indic digits pass the primary verbatim, but
    // Version.TryParse rejects them in the fallback. A supplementary digit never matches.
    EXPECT_EQ(url("apiUrl:\"https://be.wizzair.com/\u0663\u0662.1/Api\""),
              "https://be.wizzair.com/\u0663\u0662.1/Api");
    EXPECT_EQ(url("be.wizzair.com/\u0663.1/Api be.wizzair.com/2.1/Api"), "https://be.wizzair.com/2.1/Api");
    EXPECT_EQ(url("apiUrl:\"https://be.wizzair.com/\U0001D7CE.1/Api\" be.wizzair.com/3.0/Api"),
              "https://be.wizzair.com/3.0/Api");

    // Ill-formed UTF-8 matches nothing but does not disturb the scan.
    EXPECT_EQ(url("apiUrl:\xFF\"https://be.wizzair.com/1.0/Api\" \xC0\xAF be.wizzair.com/2.0/Api"),
              "https://be.wizzair.com/2.0/Api");
}

TEST(WizzProtocol, ExtractApiUrl_fallback_version_rules)
{
    // Int32 range per component (.NET).
    EXPECT_EQ(url("be.wizzair.com/2147483648.1/Api be.wizzair.com/1.0/Api"), "https://be.wizzair.com/1.0/Api");
    EXPECT_EQ(url("be.wizzair.com/2147483647.1/Api be.wizzair.com/1.0/Api"),
              "https://be.wizzair.com/2147483647.1/Api");
    // Escaped slashes are accepted around the version; "//" and a dangling '.' are not.
    EXPECT_EQ(url(R"(be.wizzair.com\/5.6\/Api)"), "https://be.wizzair.com/5.6/Api");
    EXPECT_EQ(url(R"(be.wizzair.com/5.6.\/Api)"), std::nullopt);
    EXPECT_EQ(url("be.wizzair.com//1.0/Api"), std::nullopt);
    // Equal versions: the first text wins even when written differently.
    EXPECT_EQ(url("be.wizzair.com/5.6.7.8/Api be.wizzair.com/5.6.7.08/Api"), "https://be.wizzair.com/5.6.7.8/Api");
    // Missing components count as -1: 1.0 < 1.0.0 < 1.0.0.0.
    EXPECT_EQ(url("be.wizzair.com/1.0/Api be.wizzair.com/1.0.0.0/Api be.wizzair.com/1.0.0/Api"),
              "https://be.wizzair.com/1.0.0.0/Api");
    // Lowercase host or path never matches.
    EXPECT_EQ(url("BE.WIZZAIR.COM/1.0/Api be.wizzair.com/1.0/api"), std::nullopt);
}

TEST(WizzProtocol, ExtractApiUrl_scans_a_two_megabyte_page_linearly)
{
    // The live homepage is ~1.96 MB with the config ~905 KB in (protocol-reference §3).
    std::string page;
    page.reserve(2'100'000);
    while (page.size() < 905'000) {
        page += "be.wizzair.com/apiUrl:\"https://be.wizzair.com/ apiUrl apiUrl\\\":\\\" coralogixRum.version:\"29.23.0\" ";
    }
    page += wizz::test::kHomeHtml;
    while (page.size() < 1'960'000) {
        page += "<div class=\"x\">be.wizzair.com/30.0.0/Apx</div>";
    }
    EXPECT_EQ(url(page), "https://be.wizzair.com/29.23.1/Api");
}

// ---------------------------------------------------------------------------
// System.Version (04 §7.3)
// ---------------------------------------------------------------------------

TEST(WizzProtocol, SystemVersion_parses_like_dotnet_Version_TryParse)
{
    struct Case {
        std::string_view text;
        bool ok;
        int major = 0;
        int minor = 0;
        int build = -1;
        int revision = -1;
    };
    // (.NET) Version.TryParse results.
    const std::vector<Case> cases = {
        {"29.10", true, 29, 10},
        {"29.10.0", true, 29, 10, 0},
        {"1.2.3.4", true, 1, 2, 3, 4},
        {"1.2.3.4.5", false},
        {"1", false},
        {"01.2", true, 1, 2},
        {"2147483647.0", true, 2147483647, 0},
        {"2147483648.0", false},
        {"00000000000000000002147483647.1", true, 2147483647, 1},
        {" 1.2", true, 1, 2},
        {"1 .2", true, 1, 2},
        {"1. 2", true, 1, 2},
        {"\t1.2\n", true, 1, 2},
        {"+1.2", true, 1, 2},
        {"-0.1", true, 0, 1},
        {"-1.2", false},
        {"1.-0.+3", true, 1, 0, 3},
        {"1.2.3.4 ", true, 1, 2, 3, 4},
        {"1.2.3. 4", true, 1, 2, 3, 4},
        {"1.2\0"sv, true, 1, 2},
        {"1.2\0\0"sv, true, 1, 2},
        {"1.2 \0"sv, true, 1, 2},
        {"1.2\0 "sv, false},
        {"1..2", false},
        {".1", false},
        {"1.", false},
        {"1.2.", false},
        {"1.2.3.4.", false},
        {"0x1.2", false},
        {"1.2e1", false},
        {"- 1.2", false},
        {"+-1.2", false},
        {"\u0661.\u0662", false},
        {"", false},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(std::string{c.text});
        const std::optional<SystemVersion> v = SystemVersion::try_parse(c.text);
        ASSERT_EQ(v.has_value(), c.ok);
        if (v) {
            EXPECT_EQ(v->major(), c.major);
            EXPECT_EQ(v->minor(), c.minor);
            EXPECT_EQ(v->build(), c.build);
            EXPECT_EQ(v->revision(), c.revision);
        }
    }
}

TEST(WizzProtocol, SystemVersion_compares_with_missing_components_as_minus_one)
{
    const auto v = [](std::string_view text) { return SystemVersion::try_parse(text).value(); };
    EXPECT_LT(v("29.10"), v("29.10.0"));      // (.NET) CompareTo = -1
    EXPECT_LT(v("29.10.0"), v("29.10.0.0"));
    EXPECT_LT(v("29.9.2"), v("29.10.0"));     // numeric, not textual
    EXPECT_EQ(v("29.01.0"), v("29.1.0"));
    EXPECT_GT(v("30.0"), v("29.99.99.99"));
    EXPECT_EQ(v("29.10").component_count(), 2);
    EXPECT_EQ(v("29.10.0").component_count(), 3);
    EXPECT_EQ(v("1.2.3.4").component_count(), 4);
    EXPECT_EQ(v("29.01.0").to_string(), "29.1.0");
    EXPECT_EQ(v(" +1.2.3.04 ").to_string(), "1.2.3.4");
}

// ---------------------------------------------------------------------------
// Strict api-url.txt validation (04 §7.4, D-15)
// ---------------------------------------------------------------------------

TEST(WizzProtocol, D_15_Cached_api_url_must_match_the_strict_pattern_in_full)
{
    EXPECT_TRUE(is_valid_cached_api_url("https://be.wizzair.com/29.23.1/Api"));
    EXPECT_TRUE(is_valid_cached_api_url("https://be.wizzair.com/29.22.0/Api"));
    EXPECT_TRUE(is_valid_cached_api_url("https://be.wizzair.com/1.2/Api"));
    EXPECT_TRUE(is_valid_cached_api_url("https://be.wizzair.com/1.2.3.4.5/Api"));  // (\.\d+)+ allows 5+ parts
    EXPECT_TRUE(is_valid_cached_api_url("https://be.wizzair.com/007.0/Api"));

    EXPECT_FALSE(is_valid_cached_api_url(""));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com/29/Api"));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com//Api"));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com/Api"));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com/.1/Api"));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com/1./Api"));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com/1..2/Api"));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com/29.23.1/Api/"));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com/29.23.1/ApiX"));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com/29.23.1/api"));
    EXPECT_FALSE(is_valid_cached_api_url("http://be.wizzair.com/29.23.1/Api"));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com/29.23.1/Api\n"));  // callers trim first
    EXPECT_FALSE(is_valid_cached_api_url(" https://be.wizzair.com/29.23.1/Api"));
    EXPECT_FALSE(is_valid_cached_api_url(R"(https:\/\/be.wizzair.com\/29.23.1\/Api)"));
    EXPECT_FALSE(is_valid_cached_api_url("https://bexwizzair.com/29.23.1/Api"));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com/29.-1/Api"));
    EXPECT_FALSE(is_valid_cached_api_url("https://be.wizzair.com/\u0663.1/Api"));  // ASCII digits only (D-15)

    // The frozen live api-url.txt (02 §14) is valid after Trim.
    EXPECT_TRUE(is_valid_cached_api_url(wizz::text::trim(wizz::test::read_live_cache("api-url.txt"))));
    // Every URL the extractor builds from ASCII digits passes (04 §7.1 "by construction").
    EXPECT_TRUE(is_valid_cached_api_url(url(wizz::test::kHomeHtml).value()));
}

// ---------------------------------------------------------------------------
// BuildTimetableBody (04 §9.3)
// ---------------------------------------------------------------------------

TEST(WizzProtocol, CT_APIP_07_Timetable_body_matches_the_site_format)
{
    EXPECT_EQ(build_timetable_body("bud", "ltn", Date::from_ymd(2026, 11, 10), Date::from_ymd(2026, 12, 21),
                                   PriceType::wizz_discount_club, 2),
              R"({"flightList":[{"departureStation":"BUD","arrivalStation":"LTN","from":"2026-11-10","to":"2026-12-21"}],"priceType":"wdc","adultCount":2,"childCount":0,"infantCount":0})");
    // [N] "regular" for Regular; [D] adults 0 → "adultCount":1 (also P-29).
    EXPECT_EQ(build_timetable_body("BUD", "LTN", Date::from_ymd(2026, 11, 10), Date::from_ymd(2026, 12, 21),
                                   PriceType::regular, 0),
              R"({"flightList":[{"departureStation":"BUD","arrivalStation":"LTN","from":"2026-11-10","to":"2026-12-21"}],"priceType":"regular","adultCount":1,"childCount":0,"infantCount":0})");
}

TEST(WizzProtocol, P_29_Adult_count_is_clamped_to_one)
{
    const auto body = [](int adults) {
        return build_timetable_body("BUD", "LTN", Date::from_ymd(2026, 11, 10), Date::from_ymd(2026, 11, 20),
                                    PriceType::regular, adults);
    };
    EXPECT_NE(body(0).find(R"("adultCount":1,)"), std::string::npos);
    EXPECT_NE(body(-3).find(R"("adultCount":1,)"), std::string::npos);
    EXPECT_NE(body(1).find(R"("adultCount":1,)"), std::string::npos);
    EXPECT_NE(body(9).find(R"("adultCount":9,)"), std::string::npos);
}

TEST(WizzProtocol, Timetable_body_matches_the_verified_wire_capture_04_2_3)
{
    const std::string body = build_timetable_body("BUD", "LTN", Date::from_ymd(2026, 11, 5),
                                                  Date::from_ymd(2026, 11, 7), PriceType::regular, 1);
    EXPECT_EQ(body,
              R"({"flightList":[{"departureStation":"BUD","arrivalStation":"LTN","from":"2026-11-05","to":"2026-11-07"}],"priceType":"regular","adultCount":1,"childCount":0,"infantCount":0})");
    EXPECT_EQ(body.size(), 172U);  // Content-Length: 172
}

TEST(WizzProtocol, Timetable_body_upper_cases_and_escapes_codes_without_trimming)
{
    const auto body = [](std::string_view from, std::string_view to) {
        return build_timetable_body(from, to, Date::from_ymd(2026, 1, 2), Date::from_ymd(2026, 1, 3),
                                    PriceType::regular, 1);
    };
    // JavaScriptEncoder.Default (04 §9.3, verified): upper-case \uXXXX.
    EXPECT_EQ(body("b\xC3\xBC<d", "l\"tn"),
              R"({"flightList":[{"departureStation":"B\u00DC\u003CD","arrivalStation":"L\u0022TN","from":"2026-01-02","to":"2026-01-03"}],"priceType":"regular","adultCount":1,"childCount":0,"infantCount":0})");
    // Not trimmed.
    EXPECT_NE(body(" bud ", "ltn").find(R"("departureStation":" BUD ")"), std::string::npos);
}

// ---------------------------------------------------------------------------
// ParseTimetable (04 §9.4, protocol-reference §5)
// ---------------------------------------------------------------------------

const std::vector<ExpectedRow>& bud_ltn_rows()
{
    // 05 §2.4 timetable_v2_bud_ltn.json, every row.
    static const std::vector<ExpectedRow> rows = {
        {"BUD", "LTN", 11, 10, 6, 0, "30790.0", {{6, 0}, {17, 30}}, false},
        {"BUD", "LTN", 11, 11, 6, 0, "21590.0", {{6, 0}, {8, 30}, {17, 30}}, true},
        {"BUD", "LTN", 11, 12, 6, 0, "21590.0", {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, true},
        {"BUD", "LTN", 11, 13, 12, 50, "21590.0", {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, true},
        {"BUD", "LTN", 11, 14, 6, 0, "19290.0", {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, true},
        {"BUD", "LGW", 11, 15, 8, 30, "34690.0", {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, true},
        {"BUD", "LTN", 11, 16, 6, 0, "25390.0", {{6, 0}, {8, 30}, {17, 30}}, true},
        {"BUD", "LTN", 11, 17, 6, 0, "21590.0", {{6, 0}, {17, 30}}, false},
        {"BUD", "LTN", 11, 18, 6, 0, "19290.0", {{6, 0}, {8, 30}, {17, 30}}, true},
        {"BUD", "LTN", 11, 19, 12, 50, "19290.0", {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, true},
        {"BUD", "LTN", 11, 20, 6, 0, "19290.0", {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, true},
    };
    return rows;
}

TEST(WizzProtocol, CT_APIP_03_ParseTimetable_reads_v2_rows)
{
    const std::vector<DayFare> fares = parse_timetable(wizz::test::read_fixture("timetable_v2_bud_ltn.json"));

    // [A]
    ASSERT_EQ(fares.size(), 11U);
    EXPECT_EQ(fares[0].from, "BUD");
    EXPECT_EQ(fares[0].to, "LTN");
    EXPECT_EQ(fares[0].date, Date::from_ymd(2026, 11, 10));
    EXPECT_EQ(fares[0].cheapest_departure, at(2026, 11, 10, 6, 0));
    // "Kind Unspecified": a naive local wall-clock value by type (no zone, never converted).
    static_assert(std::is_same_v<decltype(DayFare::cheapest_departure), LocalDateTime>);
    static_assert(std::is_same_v<LocalDateTime::clock, std::chrono::local_t>);
    EXPECT_EQ(fares[0].price_per_person, (Money{30790, "HUF"}));
    EXPECT_EQ(fares[0].status, FareStatus::available);
    EXPECT_EQ(fares[0].departures.size(), 2U);

    // [A] The 2026-11-15 fare goes to LGW at 08:30.
    const auto lgw = std::ranges::find(fares, Date::from_ymd(2026, 11, 15), &DayFare::date);
    ASSERT_NE(lgw, fares.end());
    EXPECT_EQ(lgw->to, "LGW");
    EXPECT_EQ(wizz::time_of_day(lgw->cheapest_departure), std::chrono::hours{8} + std::chrono::minutes{30});

    // [D] Every row of 05 §2.4, one-to-one, departures sorted, cheapest = departureDate.
    expect_rows(fares, bud_ltn_rows());
}

TEST(WizzProtocol, CT_APIP_04_ParseTimetable_marks_checkPrice_rows)
{
    const std::vector<DayFare> fares = parse_timetable(wizz::test::read_fixture("timetable_checkprice.json"));

    // [A]
    ASSERT_EQ(fares.size(), 4U);
    EXPECT_EQ(fares[0].status, FareStatus::check_on_site);
    EXPECT_TRUE(fares[0].price_per_person.amount.is_zero());
    EXPECT_EQ(fares[1].status, FareStatus::available);
    EXPECT_EQ(fares[2].status, FareStatus::available);
    EXPECT_EQ(fares[3].status, FareStatus::available);

    // [D] Fare [0] is BUD→LTN on 2026-11-02 at 17:30, Money(0, "HUF"); then 44290 (11-03 17:30),
    // 38490 to LGW (11-04 08:30) and 30790 (11-05 06:00). originalPrice 129540.00 is ignored.
    EXPECT_EQ(fares[0].price_per_person, (Money{0, "HUF"}));
    EXPECT_EQ(fares[0].price_per_person.amount.to_string(), "0");  // Money(0, ...), not the 0.0 lexeme
    expect_rows(fares, {
                           {"BUD", "LTN", 11, 2, 17, 30, "0", {{17, 30}}, false, FareStatus::check_on_site},
                           {"BUD", "LTN", 11, 3, 17, 30, "44290.0", {{6, 0}, {8, 30}, {17, 30}}, true},
                           {"BUD", "LGW", 11, 4, 8, 30, "38490.0", {{6, 0}, {8, 30}, {17, 30}}, true},
                           {"BUD", "LTN", 11, 5, 6, 0, "30790.0", {{6, 0}, {8, 30}, {12, 50}, {17, 30}}, true},
                       });
}

TEST(WizzProtocol, CT_APIP_05_ParseTimetable_skips_zero_priced_rows_and_reads_legacy_v1_strings)
{
    // V1 (05 §2.7), verbatim.
    constexpr std::string_view kV1 =
        R"({"outboundFlights":[{"departureStation":"BUD","arrivalStation":"LTN","departureDate":"2026-11-15T00:00:00","price":{"amount":19290.0,"currencyCode":"HUF"},"priceType":"price","departureDates":["2026-11-15T17:30:00","2026-11-15T06:00:00"]},{"departureStation":"BUD","arrivalStation":"LTN","departureDate":"2026-11-16T00:00:00","price":{"amount":0.0,"currencyCode":"HUF"},"priceType":"soldOut","departureDates":[]}],"returnFlights":null})";
    const std::vector<DayFare> fares = parse_timetable(kV1);

    // [A] Exactly one fare; cheapest = earliest (no flags on strings); departures sorted.
    ASSERT_EQ(fares.size(), 1U);
    EXPECT_EQ(fares[0].cheapest_departure, at(2026, 11, 15, 6, 0));
    EXPECT_EQ(fares[0].departures, (std::vector{at(2026, 11, 15, 6, 0), at(2026, 11, 15, 17, 30)}));
    EXPECT_EQ(fares[0].date, Date::from_ymd(2026, 11, 15));
    EXPECT_EQ(fares[0].price_per_person, (Money{19290, "HUF"}));
    EXPECT_EQ(fares[0].status, FareStatus::available);
    EXPECT_FALSE(fares[0].has_mac_flight);
}

TEST(WizzProtocol, CT_APIP_05_Midnight_departure_uses_the_last_flagged_entry_else_the_earliest)
{
    // Only object entries carry the flag; the LAST literally-true flag in input order wins.
    const auto cheapest = [](std::string_view dates) {
        const std::vector<DayFare> fares = parse_timetable(timetable(row_with_departure("2026-11-15T00:00:00", dates)));
        EXPECT_EQ(fares.size(), 1U);
        return fares.at(0).cheapest_departure;
    };
    EXPECT_EQ(cheapest(R"([{"date":"2026-11-15T17:30:00","isCheapestOfTheDay":true},{"date":"2026-11-15T06:00:00","isCheapestOfTheDay":true},{"date":"2026-11-15T12:00:00","isCheapestOfTheDay":false}])"),
              at(2026, 11, 15, 6, 0));
    EXPECT_EQ(cheapest(R"([{"date":"2026-11-15T06:00:00","isCheapestOfTheDay":true},{"date":"2026-11-15T17:30:00","isCheapestOfTheDay":true}])"),
              at(2026, 11, 15, 17, 30));
    EXPECT_EQ(cheapest(R"(["2026-11-15T09:00:00",{"date":"2026-11-15T17:30:00","isCheapestOfTheDay":true}])"),
              at(2026, 11, 15, 17, 30));
    // Not literally true: a string, a number, an unparsable date.
    EXPECT_EQ(cheapest(R"([{"date":"2026-11-15T17:30:00","isCheapestOfTheDay":"true"},{"date":"2026-11-15T12:00:00","isCheapestOfTheDay":1},{"date":"bad","isCheapestOfTheDay":true},"2026-11-15T08:00:00"])"),
              at(2026, 11, 15, 8, 0));
    // No departures: the midnight value stays.
    EXPECT_EQ(cheapest("[]"), at(2026, 11, 15, 0, 0));
    EXPECT_EQ(cheapest(R"(["bad",5,null,{"x":1}])"), at(2026, 11, 15, 0, 0));
    // A non-array departureDates is ignored.
    EXPECT_EQ(cheapest(R"({"date":"2026-11-15T06:00:00"})"), at(2026, 11, 15, 0, 0));
    EXPECT_EQ(cheapest(R"("2026-11-15T06:00:00")"), at(2026, 11, 15, 0, 0));
}

TEST(WizzProtocol, ParseTimetable_non_midnight_departure_is_kept_as_cheapest)
{
    const std::vector<DayFare> fares = parse_timetable(timetable(row_with_departure(
        "2026-11-15T12:50:00", R"([{"date":"2026-11-15T06:00:00","isCheapestOfTheDay":true}])")));
    ASSERT_EQ(fares.size(), 1U);
    EXPECT_EQ(fares[0].cheapest_departure, at(2026, 11, 15, 12, 50));  // not the flagged 06:00
    EXPECT_EQ(fares[0].departures, (std::vector{at(2026, 11, 15, 6, 0)}));

    // One tick after midnight is not midnight (TimeOfDay == 00:00:00.0000000 exactly).
    const std::vector<DayFare> tick = parse_timetable(
        timetable(row_with_departure("2026-11-15T00:00:00.0000001", R"(["2026-11-15T06:00:00"])")));
    ASSERT_EQ(tick.size(), 1U);
    EXPECT_EQ(tick[0].cheapest_departure, at(2026, 11, 15, 0, 0) + wizz::Ticks{1});

    // Date comes from departureDate, not from the cheapest departure.
    const std::vector<DayFare> next_day =
        parse_timetable(timetable(row_with_departure("2026-11-15T00:00:00", R"(["2026-11-16T06:00:00"])")));
    ASSERT_EQ(next_day.size(), 1U);
    EXPECT_EQ(next_day[0].date, Date::from_ymd(2026, 11, 15));
    EXPECT_EQ(next_day[0].cheapest_departure, at(2026, 11, 16, 6, 0));
}

TEST(WizzProtocol, ParseTimetable_departures_are_sorted_with_duplicates_kept)
{
    const std::vector<DayFare> fares = parse_timetable(timetable(row_with_departure(
        "2026-11-15T06:00:00",
        R"(["2026-11-15T06:00:00",{"date":"2026-11-15T05:00:00"},"2026-11-15T06:00:00",["2026-11-15T01:00:00"],{"date":5},{"x":1},true,"2026-11-15T23:59:59.9999999"])")));
    ASSERT_EQ(fares.size(), 1U);
    EXPECT_EQ(fares[0].departures,
              (std::vector{at(2026, 11, 15, 5, 0), at(2026, 11, 15, 6, 0), at(2026, 11, 15, 6, 0),
                           at(2026, 11, 15, 23, 59, 59) + wizz::Ticks{9'999'999}}));
}

TEST(WizzProtocol, CT_APIP_06_ParseTimetable_tolerates_empty_and_null_lists)
{
    // [A]
    EXPECT_TRUE(parse_timetable(R"({"outboundFlights":[],"returnFlights":null})").empty());
    EXPECT_TRUE(parse_timetable(R"({"outboundFlights":null})").empty());
    EXPECT_TRUE(parse_timetable("{}").empty());
    // Variants: any non-array outboundFlights, a differently cased key, returnFlights ignored.
    EXPECT_TRUE(parse_timetable(R"({"outboundFlights":{}})").empty());
    EXPECT_TRUE(parse_timetable(R"({"outboundFlights":"x"})").empty());
    EXPECT_TRUE(parse_timetable(R"({"outboundFlights":1})").empty());
    EXPECT_TRUE(parse_timetable(R"({"outboundFlights":true})").empty());
    const std::string row = row_with_departure("2026-11-15T06:00:00");
    EXPECT_TRUE(parse_timetable(R"({"OutboundFlights":[)" + row + "]}").empty());
    EXPECT_TRUE(parse_timetable(R"({"outboundFlights":[],"returnFlights":[)" + row + "]}").empty());
    // Duplicate keys: the last one wins.
    EXPECT_TRUE(parse_timetable(R"({"outboundFlights":[)" + row + R"(],"outboundFlights":null})").empty());
    EXPECT_EQ(parse_timetable(R"({"outboundFlights":null,"outboundFlights":[)" + row + "]}").size(), 1U);
}

TEST(WizzProtocol, ParseTimetable_rejects_malformed_shapes)
{
    // Root not an object → InvalidOperationException (json::TypeError), .NET text.
    try {
        (void)parse_timetable("[1,2,3]");
        FAIL() << "expected json::TypeError";
    } catch (const wizz::json::TypeError& e) {
        EXPECT_EQ(std::string_view{e.what()}, kTypeErrorArray);
    }
    EXPECT_THROW((void)parse_timetable("null"), wizz::json::TypeError);
    EXPECT_THROW((void)parse_timetable("\"x\""), wizz::json::TypeError);
    EXPECT_THROW((void)parse_timetable("42"), wizz::json::TypeError);

    // A non-object row rejects the whole response, even after valid rows.
    EXPECT_THROW((void)parse_timetable(R"({"outboundFlights":[1]})"), wizz::json::TypeError);
    EXPECT_THROW((void)parse_timetable(R"({"outboundFlights":[null]})"), wizz::json::TypeError);
    EXPECT_THROW((void)parse_timetable(R"({"outboundFlights":[[]]})"), wizz::json::TypeError);
    EXPECT_THROW((void)parse_timetable(timetable(row_with_departure("2026-11-15T06:00:00") + ",1")),
                 wizz::json::TypeError);

    // Not JSON → JsonException (json::ParseError): empty, text, trailing comma, comment, trailing data.
    EXPECT_THROW((void)parse_timetable(""), wizz::json::ParseError);
    EXPECT_THROW((void)parse_timetable("not json"), wizz::json::ParseError);
    EXPECT_THROW((void)parse_timetable(R"({"outboundFlights":[],})"), wizz::json::ParseError);
    EXPECT_THROW((void)parse_timetable(R"({"outboundFlights":[] /* c */})"), wizz::json::ParseError);
    EXPECT_THROW((void)parse_timetable(R"({"outboundFlights":[]} x)"), wizz::json::ParseError);
    EXPECT_THROW((void)parse_timetable("<html>Forbidden</html>"), wizz::json::ParseError);
}

TEST(WizzProtocol, ParseTimetable_amount_that_does_not_fit_a_decimal_rejects_the_response)
{
    const auto row = [](std::string_view from, std::string_view date, std::string_view amount,
                        std::string_view price_type) {
        return R"({"departureStation":)"s + std::string{from} + R"(,"arrivalStation":"LTN","departureDate":")" +
               std::string{date} + R"(","price":{"amount":)" + std::string{amount} +
               R"(,"currencyCode":"HUF"},"priceType":")" + std::string{price_type} + R"("})";
    };
    // 1e40 → FormatException (verified) with the .NET GetDecimal text; a json::Error for the caller.
    try {
        (void)parse_timetable(timetable(row(R"("BUD")", "2026-11-15T06:00:00", "1e40", "price")));
        FAIL() << "expected json::FormatError";
    } catch (const wizz::json::FormatError& e) {
        EXPECT_EQ(std::string_view{e.what()}, "One of the identified items was in an invalid format.");
    }
    EXPECT_THROW((void)parse_timetable(timetable(row(R"("BUD")", "2026-11-15T06:00:00", "-1e40", "price"))),
                 wizz::json::Error);
    EXPECT_THROW((void)parse_timetable(timetable(row(R"("BUD")", "2026-11-15T06:00:00",
                                                     "79228162514264337593543950336", "price"))),
                 wizz::json::FormatError);
    // The amount is read before the status, so a checkPrice row fails too.
    EXPECT_THROW((void)parse_timetable(timetable(row(R"("BUD")", "2026-11-15T06:00:00", "1e40", "checkPrice"))),
                 wizz::json::FormatError);
    // Rows skipped earlier (non-string station, unparsable date) never read the amount.
    EXPECT_TRUE(parse_timetable(timetable(row("null", "2026-11-15T06:00:00", "1e40", "price"))).empty());
    EXPECT_TRUE(parse_timetable(timetable(row(R"("BUD")", "garbage", "1e40", "price"))).empty());

    // Decimal edge values (.NET GetDecimal): the maximum fits; 1e-40 reads as 0 and the row is skipped.
    const std::vector<DayFare> max =
        parse_timetable(timetable(row(R"("BUD")", "2026-11-15T06:00:00", "79228162514264337593543950335", "price")));
    ASSERT_EQ(max.size(), 1U);
    EXPECT_EQ(max[0].price_per_person.amount, Decimal::max_value());
    EXPECT_TRUE(parse_timetable(timetable(row(R"("BUD")", "2026-11-15T06:00:00", "1e-40", "price"))).empty());
}

TEST(WizzProtocol, ParseTimetable_status_rules)
{
    const auto parse_one = [](std::string_view price_json, std::string_view price_type_json) {
        return parse_timetable(timetable(
            R"({"departureStation":"BUD","arrivalStation":"LTN","departureDate":"2026-11-15T06:00:00")"s +
            (price_json.empty() ? "" : R"(,"price":)" + std::string{price_json}) +
            (price_type_json.empty() ? "" : R"(,"priceType":)" + std::string{price_type_json}) + "}"));
    };
    // amount > 0 → Available, regardless of priceType.
    {
        const auto fares = parse_one(R"({"amount":10,"currencyCode":"EUR"})", R"("soldOut")");
        ASSERT_EQ(fares.size(), 1U);
        EXPECT_EQ(fares[0].status, FareStatus::available);
        EXPECT_EQ(fares[0].price_per_person, (Money{10, "EUR"}));
    }
    EXPECT_EQ(parse_one(R"({"amount":10,"currencyCode":"EUR"})", "").size(), 1U);      // no priceType
    EXPECT_EQ(parse_one(R"({"amount":10,"currencyCode":"EUR"})", "7").size(), 1U);     // non-string priceType
    // checkPrice (OrdinalIgnoreCase) → CheckOnSite with amount 0, whatever the amount.
    for (const std::string_view type : {R"("checkPrice")", R"("CHECKPRICE")", R"("checkprice")"}) {
        SCOPED_TRACE(std::string{type});
        const auto fares = parse_one(R"({"amount":129540.00,"currencyCode":"huf"})", type);
        ASSERT_EQ(fares.size(), 1U);
        EXPECT_EQ(fares[0].status, FareStatus::check_on_site);
        EXPECT_EQ(fares[0].price_per_person, (Money{0, "HUF"}));
    }
    {
        const auto fares = parse_one("", R"("checkPrice")");  // no price at all
        ASSERT_EQ(fares.size(), 1U);
        EXPECT_EQ(fares[0].price_per_person, (Money{0, ""}));
    }
    {
        const auto fares = parse_one(R"({"amount":"19290","currencyCode":"HUF"})", R"("checkPrice")");
        ASSERT_EQ(fares.size(), 1U);
        EXPECT_EQ(fares[0].status, FareStatus::check_on_site);
    }
    // Otherwise the row is dropped: 0, negative, string amount, missing / non-object price.
    EXPECT_TRUE(parse_one(R"({"amount":0.0,"currencyCode":"HUF"})", R"("price")").empty());
    EXPECT_TRUE(parse_one(R"({"amount":-5,"currencyCode":"HUF"})", R"("price")").empty());
    EXPECT_TRUE(parse_one(R"({"amount":"19290","currencyCode":"HUF"})", R"("price")").empty());
    EXPECT_TRUE(parse_one(R"({"amount":null,"currencyCode":"HUF"})", R"("price")").empty());
    EXPECT_TRUE(parse_one(R"({"currencyCode":"HUF"})", R"("price")").empty());
    EXPECT_TRUE(parse_one("", R"("price")").empty());
    EXPECT_TRUE(parse_one("null", R"("price")").empty());
    EXPECT_TRUE(parse_one("19290", R"("price")").empty());
    EXPECT_TRUE(parse_one(R"([{"amount":19290}])", R"("price")").empty());
    EXPECT_TRUE(parse_one(R"({"amount":10})", R"("noData")").size() == 1U);  // positive amount, any type
}

TEST(WizzProtocol, ParseTimetable_amounts_are_exact_decimals)
{
    const auto amount = [](std::string_view lexeme) {
        const auto fares = parse_timetable(timetable(
            R"({"departureStation":"LTN","arrivalStation":"BUD","departureDate":"2026-11-15T08:30:00","price":{"amount":)"s +
            std::string{lexeme} + R"(,"currencyCode":"GBP"},"priceType":"price"})"));
        EXPECT_EQ(fares.size(), 1U);
        return fares.at(0).price_per_person.amount;
    };
    EXPECT_EQ(amount("54.99").to_string(), "54.99");  // never through double
    EXPECT_EQ(amount("30790.0").to_string(), "30790.0");
    EXPECT_EQ(amount("1.5e2"), Decimal{150});
    EXPECT_EQ(amount("1E+2"), Decimal{100});
    EXPECT_EQ(amount("129540.00").to_string(), "129540.00");
}

TEST(WizzProtocol, ParseTimetable_row_fields_follow_the_strict_type_tests)
{
    const auto row = [](std::string_view stations, std::string_view extra = "") {
        return R"({)"s + std::string{stations} +
               R"(,"departureDate":"2026-11-15T06:00:00","price":{"amount":10,"currencyCode":"eur"},"priceType":"price")" +
               std::string{extra} + "}";
    };
    // Codes and currency upper-cased (invariant), never trimmed; currency upper-cased.
    {
        const auto fares = parse_timetable(timetable(row(R"("departureStation":" bud ","arrivalStation":"lgw")")));
        ASSERT_EQ(fares.size(), 1U);
        EXPECT_EQ(fares[0].from, " BUD ");
        EXPECT_EQ(fares[0].to, "LGW");
        EXPECT_EQ(fares[0].price_per_person.currency, "EUR");
    }
    // Str() = only JSON strings: a non-string or missing station skips the row.
    EXPECT_TRUE(parse_timetable(timetable(row(R"("departureStation":1,"arrivalStation":"LTN")"))).empty());
    EXPECT_TRUE(parse_timetable(timetable(row(R"("departureStation":"BUD","arrivalStation":null)"))).empty());
    EXPECT_TRUE(parse_timetable(timetable(row(R"("departureStation":"BUD")"))).empty());
    EXPECT_TRUE(parse_timetable(timetable(row(R"("DepartureStation":"BUD","arrivalStation":"LTN")"))).empty());
    // A non-string departureDate skips the row.
    EXPECT_TRUE(parse_timetable(timetable(
                                    R"({"departureStation":"BUD","arrivalStation":"LTN","departureDate":20261115,"price":{"amount":10},"priceType":"price"})"))
                    .empty());
    // Missing or non-string currency → "".
    {
        const auto fares = parse_timetable(timetable(
            R"({"departureStation":"BUD","arrivalStation":"LTN","departureDate":"2026-11-15T06:00:00","price":{"amount":10,"currencyCode":5},"priceType":"price"})"));
        ASSERT_EQ(fares.size(), 1U);
        EXPECT_EQ(fares[0].price_per_person.currency, "");
    }
    // hasMacFlight only when literally true.
    const auto mac = [&](std::string_view value) {
        const auto fares = parse_timetable(
            timetable(row(R"("departureStation":"BUD","arrivalStation":"LTN")", R"(,"hasMacFlight":)" + std::string{value})));
        EXPECT_EQ(fares.size(), 1U);
        return fares.at(0).has_mac_flight;
    };
    EXPECT_TRUE(mac("true"));
    EXPECT_FALSE(mac("false"));
    EXPECT_FALSE(mac(R"("true")"));
    EXPECT_FALSE(mac("1"));
    EXPECT_FALSE(mac("null"));
    // Duplicate keys: the last value wins (verified), for stations, price and flags alike.
    {
        const auto fares = parse_timetable(timetable(
            R"({"departureStation":"XXX","departureStation":"bud","arrivalStation":"LTN","departureDate":"bad","departureDate":"2026-11-15T06:00:00","price":{"amount":1,"currencyCode":"EUR"},"price":{"amount":2,"amount":3,"currencyCode":"GBP"},"priceType":"checkPrice","priceType":"price","hasMacFlight":true,"hasMacFlight":false})"));
        ASSERT_EQ(fares.size(), 1U);
        EXPECT_EQ(fares[0].from, "BUD");
        EXPECT_EQ(fares[0].price_per_person, (Money{3, "GBP"}));
        EXPECT_EQ(fares[0].status, FareStatus::available);
        EXPECT_FALSE(fares[0].has_mac_flight);
    }
}

TEST(WizzProtocol, ParseTimetable_keeps_response_order_without_dedupe)
{
    const std::string later = row_with_departure("2026-11-16T06:00:00");
    const std::string earlier = row_with_departure("2026-11-15T06:00:00");
    const std::vector<DayFare> fares =
        parse_timetable(R"({"outboundFlights":[)" + later + "," + earlier + "," + earlier + "]}");
    ASSERT_EQ(fares.size(), 3U);
    EXPECT_EQ(fares[0].date, Date::from_ymd(2026, 11, 16));
    EXPECT_EQ(fares[1].date, Date::from_ymd(2026, 11, 15));
    EXPECT_EQ(fares[2].date, Date::from_ymd(2026, 11, 15));
}

TEST(WizzProtocol, ParseTimetable_reads_the_inline_B1_body)
{
    // B1 (05 §2.7), verbatim.
    constexpr std::string_view kB1 =
        R"({"outboundFlights":[{"departureStation":"BTS","arrivalStation":"LTN","departureDate":"2026-11-12T07:15:00","price":{"amount":14.99,"currencyCode":"EUR"},"priceType":"price","departureDates":[{"date":"2026-11-12T07:15:00","isCheapestOfTheDay":true}],"hasMacFlight":false}],"returnFlights":null})";
    const std::vector<DayFare> fares = parse_timetable(kB1);
    ASSERT_EQ(fares.size(), 1U);
    EXPECT_EQ(fares[0].from, "BTS");
    EXPECT_EQ(fares[0].to, "LTN");
    EXPECT_EQ(fares[0].date, Date::from_ymd(2026, 11, 12));
    EXPECT_EQ(fares[0].cheapest_departure, at(2026, 11, 12, 7, 15));
    EXPECT_EQ(fares[0].departures, (std::vector{at(2026, 11, 12, 7, 15)}));
    EXPECT_EQ(fares[0].price_per_person, (Money{dec("14.99"), "EUR"}));
    EXPECT_EQ(fares[0].status, FareStatus::available);
    EXPECT_FALSE(fares[0].has_mac_flight);
}

/// 04 §9.4 TryParseLocal table, applied to `departureDate` (row parsed or skipped) and to a
/// V1 `departureDates` string (entry kept or ignored). D-22: offsets keep the wall clock.
TEST(WizzProtocol, ParseTimetable_date_parse_table_04_9_4)
{
    struct Case {
        std::string_view text;
        std::optional<LocalDateTime> expected;
    };
    const Date nov15 = Date::from_ymd(2026, 11, 15);
    const std::vector<Case> cases = {
        {"2026-11-15T08:30:00", at(2026, 11, 15, 8, 30)},
        {"2026-11-15T08:30:00.123", at(2026, 11, 15, 8, 30) + std::chrono::milliseconds{123}},
        {"2026-11-15T08:30", at(2026, 11, 15, 8, 30)},
        {"2026-11-15 08:30:00", at(2026, 11, 15, 8, 30)},
        {"2026-11-15", wizz::at_midnight(nov15)},
        {" 2026-11-15T08:30:00 ", at(2026, 11, 15, 8, 30)},
        {"11/15/2026", wizz::at_midnight(nov15)},
        {"15/11/2026", std::nullopt},
        {"2026-11-15T24:00:00", std::nullopt},
        {"2026-02-30T00:00:00", std::nullopt},
        {"", std::nullopt},
        {"garbage", std::nullopt},
        {"2026-11-15T08:00:00Z", at(2026, 11, 15, 8, 0)},       // D-22: wall clock kept
        {"2026-11-15T08:00:00+02:00", at(2026, 11, 15, 8, 0)},  // D-22: wall clock kept
    };
    for (const Case& c : cases) {
        SCOPED_TRACE("\"" + std::string{c.text} + "\"");
        const std::vector<DayFare> row = parse_timetable(timetable(row_with_departure(c.text)));
        if (c.expected) {
            ASSERT_EQ(row.size(), 1U);
            EXPECT_EQ(row[0].date, wizz::date_of(*c.expected));
            EXPECT_EQ(row[0].cheapest_departure, *c.expected);  // no departures: departureDate itself
        } else {
            EXPECT_TRUE(row.empty());
        }

        wizz::json::Writer dates;
        dates.begin_array().string(c.text).end_array();
        const std::vector<DayFare> v1 =
            parse_timetable(timetable(row_with_departure("2026-11-15T00:00:00", dates.take())));
        ASSERT_EQ(v1.size(), 1U);
        if (c.expected) {
            EXPECT_EQ(v1[0].departures, (std::vector{*c.expected}));
            EXPECT_EQ(v1[0].cheapest_departure, *c.expected);
        } else {
            EXPECT_TRUE(v1[0].departures.empty());
            EXPECT_EQ(v1[0].cheapest_departure, wizz::at_midnight(nov15));
        }
    }
}

TEST(WizzProtocol, P_30_Parses_the_BUD_NAP_fixture)
{
    const std::vector<DayFare> fares = parse_timetable(wizz::test::read_fixture("timetable_v2_bud_nap.json"));
    // 10 rows, 12-01..12-10, the first 11590 HUF at 21:05 (05 §2.4, P-30).
    const auto nap = [](unsigned day, int hour, int minute, std::string_view amount,
                        std::vector<std::pair<int, int>> departures) {
        return ExpectedRow{"BUD", "NAP", 12, day, hour, minute, amount, std::move(departures), false};
    };
    expect_rows(fares, {
                           nap(1, 21, 5, "11590.0", {{21, 5}}),
                           nap(2, 14, 20, "12290.0", {{14, 20}}),
                           nap(3, 14, 20, "12290.0", {{14, 20}}),
                           nap(4, 8, 30, "12290.0", {{8, 30}, {13, 25}}),
                           nap(5, 14, 20, "13890.0", {{14, 20}}),
                           nap(6, 14, 20, "25390.0", {{14, 20}}),
                           nap(7, 9, 30, "15390.0", {{9, 30}, {14, 20}}),
                           nap(8, 14, 20, "92490.0", {{14, 20}}),
                           nap(9, 14, 20, "57790.0", {{14, 20}}),
                           nap(10, 14, 20, "26990.0", {{14, 20}}),
                       });
    ASSERT_FALSE(fares.empty());
    EXPECT_EQ(fares.front().price_per_person, (Money{11590, "HUF"}));
    EXPECT_EQ(fares.front().cheapest_departure, at(2026, 12, 1, 21, 5));
}

// ---------------------------------------------------------------------------
// ParseValidationCodes, LooksLikeJson, Snippet (04 §11.5)
// ---------------------------------------------------------------------------

TEST(WizzProtocol, Unreadable_validation_codes_count_as_none_D_29)
{
    // A lone surrogate makes the code (or a member name) unreadable (D-28); the
    // body must then count as "no codes" instead of escaping the client.
    EXPECT_TRUE(parse_validation_codes("{\"validationCodes\":[\"Invalid\\ud800Market\"]}").empty());
    // Lookups scan members last to first (D-28): a bad name after the match is
    // reached first and makes the body unreadable; one before it is never read.
    EXPECT_TRUE(parse_validation_codes("{\"validationCodes\":[\"InvalidMarket\"],\"\\udc00\":1}").empty());
    EXPECT_EQ(parse_validation_codes("{\"\\udc00\":1,\"validationCodes\":[\"InvalidMarket\"]}"),
              (std::vector<std::string>{"InvalidMarket"}));
}

TEST(WizzProtocol, CT_APIP_08_Validation_codes_are_parsed)
{
    // [A]
    EXPECT_EQ(parse_validation_codes(wizz::test::read_fixture("error_invalid_market.json")),
              (std::vector<std::string>{"InvalidMarket"}));
    EXPECT_EQ(parse_validation_codes(wizz::test::read_fixture("error_invalid_range.json")),
              (std::vector<std::string>{"InvalidTimeDateRange"}));
    EXPECT_TRUE(parse_validation_codes("not json").empty());
    // [N] Only string elements are kept (empty strings too), in order; a non-object root gives [].
    EXPECT_EQ(parse_validation_codes(R"({"validationCodes":["InvalidFromDate",1,null,"",{"x":1},["y"],true,"InvalidMarket"]})"),
              (std::vector<std::string>{"InvalidFromDate", "", "InvalidMarket"}));
    EXPECT_TRUE(parse_validation_codes(R"(["InvalidMarket"])").empty());
    EXPECT_TRUE(parse_validation_codes(R"("InvalidMarket")").empty());
    EXPECT_TRUE(parse_validation_codes("null").empty());
    EXPECT_TRUE(parse_validation_codes("").empty());
    EXPECT_TRUE(parse_validation_codes("{}").empty());
    EXPECT_TRUE(parse_validation_codes(R"({"validationCodes":"InvalidMarket"})").empty());
    EXPECT_TRUE(parse_validation_codes(R"({"validationCodes":null})").empty());
    EXPECT_TRUE(parse_validation_codes(R"({"ValidationCodes":["InvalidMarket"]})").empty());
    EXPECT_TRUE(parse_validation_codes(R"({"validationCodes":["InvalidMarket"],})").empty());  // invalid JSON
    EXPECT_TRUE(parse_validation_codes(R"({"x":1})").empty());
    // Duplicate key: the last wins.
    EXPECT_EQ(parse_validation_codes(R"({"validationCodes":["A"],"validationCodes":["B"]})"),
              (std::vector<std::string>{"B"}));
    // Escapes are decoded.
    EXPECT_EQ(parse_validation_codes(R"({"validationCodes":["Invalid\u004Darket"]})"),
              (std::vector<std::string>{"InvalidMarket"}));
}

TEST(WizzProtocol, LooksLikeJson_checks_the_first_non_white_space_character)
{
    EXPECT_TRUE(looks_like_json("{}"));
    EXPECT_TRUE(looks_like_json("[1,2,3]"));
    EXPECT_TRUE(looks_like_json(" \t\r\n{\"x\":1}"));
    EXPECT_TRUE(looks_like_json("\u00A0\u2028["));  // Unicode white space is trimmed
    EXPECT_TRUE(looks_like_json("{not really json"));
    EXPECT_FALSE(looks_like_json(""));
    EXPECT_FALSE(looks_like_json("   "));
    EXPECT_FALSE(looks_like_json("\"text\""));
    EXPECT_FALSE(looks_like_json("null"));
    EXPECT_FALSE(looks_like_json("<html>Forbidden</html>"));
    EXPECT_FALSE(looks_like_json("busy"));
    EXPECT_FALSE(looks_like_json("x{"));
    EXPECT_FALSE(looks_like_json("\uFEFF{}"));  // U+FEFF is not white space
    EXPECT_FALSE(looks_like_json("\u200B[]"));  // nor is U+200B
}

TEST(WizzProtocol, Snippet_cuts_after_200_UTF16_code_units_with_an_ellipsis)
{
    const std::string ellipsis = "\u2026";
    EXPECT_EQ(snippet(""), "");
    EXPECT_EQ(snippet(R"({"x":1})"), R"({"x":1})");
    const std::string a200(200, 'a');
    EXPECT_EQ(snippet(a200), a200);
    EXPECT_EQ(snippet(a200 + "b"), a200 + ellipsis);
    EXPECT_EQ(snippet(std::string(5000, 'z')), std::string(200, 'z') + ellipsis);

    // Lengths count UTF-16 code units: 200 two-byte characters are 400 bytes but 200 units.
    std::string e200;
    for (int i = 0; i < 200; ++i) {
        e200 += "\u00E9";
    }
    EXPECT_EQ(snippet(e200), e200);
    EXPECT_EQ(snippet(e200 + "\u00E9"), e200 + ellipsis);
    std::string cjk;
    for (int i = 0; i < 300; ++i) {
        cjk += "\u65E5";
    }
    std::string cjk200;
    for (int i = 0; i < 200; ++i) {
        cjk200 += "\u65E5";
    }
    EXPECT_EQ(snippet(cjk), cjk200 + ellipsis);

    // A supplementary character is two units and is never split.
    const std::string emoji = "\U0001F600";
    EXPECT_EQ(snippet(std::string(198, 'a') + emoji), std::string(198, 'a') + emoji);            // exactly 200
    EXPECT_EQ(snippet(std::string(199, 'a') + emoji), std::string(199, 'a') + ellipsis);         // would split
    EXPECT_EQ(snippet(std::string(198, 'a') + emoji + "b"), std::string(198, 'a') + emoji + ellipsis);
    std::string emojis;
    for (int i = 0; i < 101; ++i) {
        emojis += emoji;
    }
    std::string emojis100;
    for (int i = 0; i < 100; ++i) {
        emojis100 += emoji;
    }
    EXPECT_EQ(snippet(emojis), emojis100 + ellipsis);
}

TEST(WizzProtocol, P_06_Snippet_of_a_long_JSON_error_body)
{
    // P-06 (b): the first 200 characters of the body followed by "…" (U+2026).
    std::string body = R"({"message":")";
    while (body.size() < 260) {
        body += "No HTTP resource was found that matches the request URI. ";
    }
    body += R"("})";
    const std::string s = snippet(body);
    EXPECT_EQ(s, body.substr(0, 200) + "\xE2\x80\xA6");
    EXPECT_EQ(wizz::text::utf16_length(s), 201U);
}

} // namespace
