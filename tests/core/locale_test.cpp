// The locale service of 07 §1: number symbols, names, .NET custom date/time
// patterns (01, 03 §0), collation (02 §5.2–5.3, 05 §2.3, CT-CAT-27),
// culture-aware prefix matching (03 §5), title case (02 §5.6) and casing.

#include "wizz/core/locale.hpp"
#include "wizz/core/text.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::string_view_literals;
using wizz::CollationLess;
using wizz::CollationStrength;
using wizz::Date;
using wizz::Locale;
using wizz::NameContext;
using wizz::NumberSymbols;

const Locale& en_us()
{
    return *Locale::en_us();
}

const Locale& locale(std::string_view name)
{
    // Keep the created locales alive for the whole test binary.
    static std::vector<std::shared_ptr<const Locale>> cache;
    for (const auto& existing : cache) {
        if (existing->name() == name) {
            return *existing;
        }
    }
    return *cache.emplace_back(Locale::create(name));
}

std::vector<std::string> sorted(std::vector<std::string> names, const Locale& culture, CollationStrength strength)
{
    std::ranges::stable_sort(names, CollationLess{culture, strength});
    return names;
}

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

TEST(Locale, En_us_is_a_shared_instance)
{
    EXPECT_EQ(Locale::en_us().get(), Locale::en_us().get());
    EXPECT_EQ(en_us().name(), "en-US");
}

TEST(Locale, Rejects_malformed_names)
{
    EXPECT_THROW((void)Locale::create(""), std::invalid_argument);
    EXPECT_THROW((void)Locale::create("not a locale"), std::invalid_argument);
    EXPECT_THROW((void)Locale::create("en_US"), std::invalid_argument); // BCP-47 uses '-'
}

TEST(Locale, User_default_is_usable)
{
    const auto user = Locale::user_default();
    ASSERT_NE(user, nullptr);
    EXPECT_FALSE(user->name().empty());
    EXPECT_FALSE(user->number_symbols().decimal_separator.empty());
    EXPECT_FALSE(user->number_symbols().group_sizes.empty());
    EXPECT_FALSE(user->month_abbrev(11).empty());
    EXPECT_FALSE(user->format_date(Date::from_ymd(2026, 11, 20), "ddd dd MMM yyyy").empty());
}

// ---------------------------------------------------------------------------
// Number symbols and time separator
// ---------------------------------------------------------------------------

TEST(LocaleNumbers, En_us_symbols)
{
    EXPECT_EQ(en_us().number_symbols(), (NumberSymbols{".", ",", "-", {3}}));
    EXPECT_EQ(en_us().time_separator(), ":");
}

TEST(LocaleNumbers, De_de_symbols)
{
    EXPECT_EQ(locale("de-DE").number_symbols(), (NumberSymbols{",", ".", "-", {3}}));
    EXPECT_EQ(locale("de-DE").time_separator(), ":");
}

TEST(LocaleNumbers, Other_cultures_follow_dotnet_on_icu)
{
    // Values recorded with .NET 10 (ICU) NumberFormatInfo / DateTimeFormatInfo.
    EXPECT_EQ(locale("en-IN").number_symbols(), (NumberSymbols{".", ",", "-", {3, 2}}));
    EXPECT_EQ(locale("sv-SE").number_symbols(), (NumberSymbols{",", " ", "−", {3}}));
    EXPECT_EQ(locale("fr-FR").number_symbols(), (NumberSymbols{",", " ", "-", {3}}));
    EXPECT_EQ(locale("fi-FI").number_symbols(), (NumberSymbols{",", " ", "−", {3}}));
    EXPECT_EQ(locale("de-CH").number_symbols(), (NumberSymbols{".", "’", "-", {3}}));
    EXPECT_EQ(locale("fi-FI").time_separator(), ".");
    EXPECT_EQ(locale("fr-CA").time_separator(), ":"); // .NET special case
    EXPECT_EQ(locale("de-CH").time_separator(), ":");
}

// ---------------------------------------------------------------------------
// Month and weekday names
// ---------------------------------------------------------------------------

TEST(LocaleNames, En_us_abbreviations)
{
    const std::vector<std::string> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (unsigned m = 1; m <= 12; ++m) {
        EXPECT_EQ(en_us().month_abbrev(m), months[m - 1]);
        EXPECT_EQ(en_us().month_abbrev(m, NameContext::standalone), months[m - 1]);
    }
    const std::vector<std::string> days{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    for (unsigned d = 0; d < 7; ++d) {
        EXPECT_EQ(en_us().day_abbrev(std::chrono::weekday{d}), days[d]);
    }
}

TEST(LocaleNames, De_de_format_and_standalone_months_differ)
{
    const std::vector<std::string> format{"Jan.", "Feb.", "März", "Apr.", "Mai", "Juni",
                                          "Juli", "Aug.", "Sept.", "Okt.", "Nov.", "Dez."};
    const std::vector<std::string> standalone{"Jan", "Feb", "Mär", "Apr", "Mai", "Jun",
                                              "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"};
    const Locale& de = locale("de-DE");
    for (unsigned m = 1; m <= 12; ++m) {
        EXPECT_EQ(de.month_abbrev(m), format[m - 1]);
        EXPECT_EQ(de.month_abbrev(m, NameContext::standalone), standalone[m - 1]);
    }
    // .NET AbbreviatedDayNames are the stand-alone forms (no dots in de).
    const std::vector<std::string> days{"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
    for (unsigned d = 0; d < 7; ++d) {
        EXPECT_EQ(de.day_abbrev(std::chrono::weekday{d}), days[d]);
    }
}

TEST(LocaleNames, En_gb_september_is_sept)
{
    EXPECT_EQ(locale("en-GB").format_date(Date::from_ymd(2026, 9, 7), "d MMM"), "7 Sept");
}

TEST(LocaleNames, Out_of_range_arguments_throw)
{
    EXPECT_THROW((void)en_us().month_abbrev(0), std::out_of_range);
    EXPECT_THROW((void)en_us().month_abbrev(13), std::out_of_range);
    EXPECT_THROW((void)en_us().day_abbrev(std::chrono::weekday{8}), std::out_of_range);
}

// ---------------------------------------------------------------------------
// .NET custom date/time patterns
// ---------------------------------------------------------------------------

TEST(LocaleDateFormat, Picker_and_grid_dates_01)
{
    // 01 §4.2/§4.4 date pickers and the grid's Date column: "ddd dd MMM yyyy".
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2026, 11, 20), "ddd dd MMM yyyy"), "Fri 20 Nov 2026");
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2026, 12, 5), "ddd dd MMM yyyy"), "Sat 05 Dec 2026");
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2026, 12, 6), "ddd dd MMM yyyy"), "Sun 06 Dec 2026");
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2026, 12, 20), "ddd dd MMM yyyy"), "Sun 20 Dec 2026");
    // Round-trip grid columns and summaries: "ddd dd MMM".
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2026, 12, 5), "ddd dd MMM"), "Sat 05 Dec");
}

TEST(LocaleDateFormat, Engine_date_fragments_03)
{
    // 03 §0: {d:d MMM} and {d:dd MMM yyyy}.
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2026, 11, 10), "d MMM"), "10 Nov");
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2026, 11, 2), "d MMM"), "2 Nov");
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2027, 6, 7), "dd MMM yyyy"), "07 Jun 2027");
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2026, 12, 1), "dd MMM yyyy"), "01 Dec 2026"); // "next flight 01 Dec 2026"
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2026, 11, 10), "dd MMM"), "10 Nov");
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2026, 11, 2), "yyyy-MM-dd"), "2026-11-02");
    EXPECT_EQ(en_us().format_date(Date{}, "yyyy-MM-dd"), "0001-01-01");
}

TEST(LocaleDateFormat, Times_use_the_culture_time_separator)
{
    const auto evening = wizz::make_local(Date::from_ymd(2026, 12, 5), 20, 40);
    const auto morning = wizz::make_local(Date::from_ymd(2026, 11, 2), 8, 5, 9);
    EXPECT_EQ(en_us().format_time(evening, "HH:mm"), "20:40");
    EXPECT_EQ(en_us().format_time(morning, "HH:mm:ss"), "08:05:09");
    EXPECT_EQ(en_us().format_time(morning, "H:m:s"), "8:5:9");
    EXPECT_EQ(en_us().format_time(morning, "yyyy-MM-ddTHH:mm:ss"), "2026-11-02T08:05:09");
    EXPECT_EQ(locale("fi-FI").format_time(evening, "HH:mm"), "20.40");
    // 01 §8.9 copy line: "{Date:ddd dd MMM yyyy} {CheapestDeparture:HH:mm}".
    EXPECT_EQ(en_us().format_date(Date::from_ymd(2026, 12, 5), "ddd dd MMM yyyy") + " " +
                  en_us().format_time(evening, "HH:mm"),
              "Sat 05 Dec 2026 20:40");
}

TEST(LocaleDateFormat, Literals_escapes_and_short_tokens)
{
    const Date date = Date::from_ymd(2005, 3, 7);
    EXPECT_EQ(en_us().format_date(date, "dd 'of' MMM"), "07 of Mar");
    EXPECT_EQ(en_us().format_date(date, "\"day\" d"), "day 7");
    EXPECT_EQ(en_us().format_date(date, "'it''s' d"), "its 7");
    EXPECT_EQ(en_us().format_date(date, "'a\\'b' d"), "a'b 7");
    EXPECT_EQ(en_us().format_date(date, "d\\d"), "7d");
    EXPECT_EQ(en_us().format_date(date, "%d"), "7");
    EXPECT_EQ(en_us().format_date(date, "%M"), "3");
    EXPECT_EQ(en_us().format_date(date, "y yy yyy yyyyy"), "5 05 2005 02005");
    EXPECT_EQ(en_us().format_date(date, "M.MM"), "3.03");
    EXPECT_EQ(en_us().format_date(date, "d – MMM →"), "7 – Mar →");
    EXPECT_EQ(en_us().format_time(wizz::make_local(date, 9, 7), "HHH:mmm"), "09:07"); // capped at 2 digits
}

TEST(LocaleDateFormat, Unsupported_or_malformed_patterns_throw)
{
    const Date date = Date::from_ymd(2026, 11, 2);
    for (const std::string_view pattern : {"dddd", "MMMM", "hh:mm", "HH:mm tt", "dd/MM", "fff", "zzz", "K",
                                           "'open", "d\\", "%", "%%"}) {
        EXPECT_THROW((void)en_us().format_date(date, pattern), std::invalid_argument) << pattern;
    }
}

TEST(LocaleDateFormat, Month_form_follows_dotnet_genitive_rule)
{
    // Expected values recorded with .NET 10 DateTime.ToString(pattern, de-DE).
    const Locale& de = locale("de-DE");
    // A d/dd token next to MMM selects the format form…
    EXPECT_EQ(de.format_date(Date::from_ymd(2026, 11, 20), "ddd dd MMM yyyy"), "Fr 20 Nov. 2026");
    EXPECT_EQ(de.format_date(Date::from_ymd(2027, 6, 7), "ddd dd MMM yyyy"), "Mo 07 Juni 2027");
    EXPECT_EQ(de.format_date(Date::from_ymd(2026, 11, 20), "d MMM"), "20 Nov.");
    EXPECT_EQ(de.format_date(Date::from_ymd(2027, 6, 7), "d MMM"), "7 Juni");
    EXPECT_EQ(de.format_date(Date::from_ymd(2026, 11, 20), "MMM d"), "Nov. 20");
    EXPECT_EQ(de.format_date(Date::from_ymd(2027, 6, 7), "MMM d"), "Juni 7");
    // …otherwise (none, or only ddd) the stand-alone form.
    EXPECT_EQ(de.format_date(Date::from_ymd(2026, 11, 20), "MMM yyyy"), "Nov 2026");
    EXPECT_EQ(de.format_date(Date::from_ymd(2027, 6, 7), "MMM yyyy"), "Jun 2027");
    EXPECT_EQ(de.format_date(Date::from_ymd(2026, 11, 20), "ddd MMM"), "Fr Nov");
    EXPECT_EQ(de.format_date(Date::from_ymd(2027, 6, 7), "ddd MMM"), "Mo Jun");
    EXPECT_EQ(de.format_date(Date::from_ymd(2027, 6, 7), "dd.MM.yyyy"), "07.06.2027");
}

// ---------------------------------------------------------------------------
// Collation
// ---------------------------------------------------------------------------

TEST(LocaleCollation, Real_airports_order_05_2_3)
{
    // Map order of the 11 real stations of map_subset.json; RealAirports is a
    // stable culture sort by Name (02 §5.2).
    const std::vector<std::string> map_order{"Tirana",   "Budapest", "Debrecen",          "Alghero (Sardinia)",
                                             "Naples",   "Katowice", "Kraków",        "Bratislava",
                                             "Barcelona El Prat", "London Luton", "London Gatwick"};
    EXPECT_EQ(sorted(map_order, en_us(), CollationStrength::tertiary),
              (std::vector<std::string>{"Alghero (Sardinia)", "Barcelona El Prat", "Bratislava", "Budapest",
                                        "Debrecen", "Katowice", "Kraków", "London Gatwick", "London Luton",
                                        "Naples", "Tirana"}));
}

TEST(LocaleCollation, Stable_sort_keeps_map_order_for_equal_names)
{
    // BAS → MLH, BSL: both carry "Basel-Mulhouse-Freiburg" (02 §5.2).
    std::vector<std::pair<std::string, std::string>> stations{
        {"MLH", "Basel-Mulhouse-Freiburg"}, {"BER", "Berlin"}, {"BSL", "Basel-Mulhouse-Freiburg"}};
    const CollationLess less{en_us()};
    std::ranges::stable_sort(stations, [&](const auto& a, const auto& b) { return less(a.second, b.second); });
    EXPECT_EQ(stations[0].first, "MLH");
    EXPECT_EQ(stations[1].first, "BSL");
    EXPECT_EQ(stations[2].first, "BER");
}

TEST(LocaleCollation, CT_CAT_27_Suggestion_names_sorted_set)
{
    // SuggestionNames: SortedSet(StringComparer.CurrentCultureIgnoreCase), All in map
    // order, pseudo stations contribute their city name.
    std::set<std::string, CollationLess> names{CollationLess{en_us(), CollationStrength::secondary}};
    for (const char* name : {"Tirana", "Budapest", "Debrecen", "Alghero (Sardinia)", "Naples", "Katowice",
                             "Kraków", "Bratislava", "Barcelona El Prat", "London", "London Luton",
                             "London Gatwick"}) {
        names.insert(name);
    }
    const std::vector<std::string> expected{"Alghero (Sardinia)", "Barcelona El Prat", "Bratislava", "Budapest",
                                            "Debrecen",           "Katowice",          "Kraków", "London",
                                            "London Gatwick",     "London Luton",      "Naples",     "Tirana"};
    EXPECT_EQ(std::vector<std::string>(names.begin(), names.end()), expected);
    EXPECT_TRUE(names.contains("London"));
    EXPECT_TRUE(names.contains("London Luton"));
    EXPECT_TRUE(names.contains("Kraków"));
    EXPECT_FALSE(names.contains("London (All Airports)"));

    // Duplicates are removed case-insensitively; accents still count.
    EXPECT_FALSE(names.insert("LONDON").second);
    EXPECT_FALSE(names.insert("kraków").second);
    EXPECT_TRUE(names.insert("Krakow").second);
    EXPECT_EQ(*names.find("london"), "London"); // the first spelling is kept
}

TEST(LocaleCollation, Live_suggestion_list_starts_like_02_5_3)
{
    const std::vector<std::string> shuffled{"Almería", "Alicante", "Agadir", "Alghero (Sardinia)", "Aberdeen",
                                            "Alexandria", "Abu Dhabi", "Alesund"};
    EXPECT_EQ(sorted(shuffled, en_us(), CollationStrength::secondary),
              (std::vector<std::string>{"Aberdeen", "Abu Dhabi", "Agadir", "Alesund", "Alexandria",
                                        "Alghero (Sardinia)", "Alicante", "Almería"}));
}

TEST(LocaleCollation, Strengths_map_to_dotnet_compare_options)
{
    // None: case and accents matter; ICU puts lower case first.
    EXPECT_LT(en_us().compare("london", "London"), 0);
    EXPECT_LT(en_us().compare("Krakow", "Kraków"), 0);
    EXPECT_EQ(en_us().compare("Budapest", "Budapest"), 0);
    // IgnoreCase: accents still matter.
    EXPECT_EQ(en_us().compare("london", "LONDON", CollationStrength::secondary), 0);
    EXPECT_NE(en_us().compare("Krakow", "Kraków", CollationStrength::secondary), 0);
    // IgnoreCase | IgnoreNonSpace.
    EXPECT_EQ(en_us().compare("KRAKOW", "Kraków", CollationStrength::primary), 0);
    // Linguistic, not ordinal: 'a' < 'B' although 'B' (0x42) < 'a' (0x61).
    EXPECT_LT(en_us().compare("a", "B"), 0);
    EXPECT_LT(en_us().compare("", "a"), 0);
    EXPECT_EQ(en_us().compare("", ""), 0);
    // Canonically equivalent spellings are equal.
    EXPECT_EQ(en_us().compare("Kraków", "Kraków"), 0);
}

TEST(LocaleCollation, Ignore_case_keeps_kana_type_and_width_like_dotnet)
{
    // .NET adds custom rules below tertiary strength so that IgnoreCase does
    // not also ignore half/full width or hiragana/katakana. Signs recorded with
    // .NET 10 CompareInfo.Compare (en-US).
    EXPECT_LT(en_us().compare("a", "ａ", CollationStrength::secondary), 0);
    EXPECT_EQ(en_us().compare("Ａ", "ａ", CollationStrength::secondary), 0);
    EXPECT_LT(en_us().compare("あ", "ア", CollationStrength::secondary), 0);
    EXPECT_GT(en_us().compare("ｱ", "ア", CollationStrength::primary), 0);
    EXPECT_LT(en_us().compare("a", "ａ"), 0);

    struct Case {
        std::string_view a;
        std::string_view b;
        int none;
        int ignore_case;
        int ignore_case_and_non_space;
    };
    const Case cases[] = {
        {"Ａb", "ａc", -1, -1, -1},
        {"ｚ", "Ｚ", -1, 0, 0},  // full-width z / Z: case only
        {"ａ", "Ａ", -1, 0, 0},
        {"ｶ", "カ", 1, 1, 1},   // half-width vs full-width katakana
        {"ガ", "ｶﾞ", -1, -1, -1},
        {"か", "カ", -1, -1, -1}, // hiragana vs katakana
        {"０", "0", 1, 1, 1},
        {"！", "!", 1, 1, 1},
        {"￥", "¥", 1, 1, 1},
        {"　", " ", 1, 0, 0},         // ideographic space is not in the table
        {"Ａ", "A", 1, 1, 1},
        {"Ａ", "a", 1, 1, 1},
        {"ａ", "A", -1, 1, 1},
        {"ＡＢ", "ａｂ", 1, 0, 0},
        {"＼", "\\", 1, 0, 0},        // U+FF3C is not in the table either
    };
    const auto sign = [](int value) { return (value > 0) - (value < 0); };
    for (const Case& c : cases) {
        EXPECT_EQ(sign(en_us().compare(c.a, c.b, CollationStrength::tertiary)), c.none) << c.a << " / " << c.b;
        EXPECT_EQ(sign(en_us().compare(c.a, c.b, CollationStrength::secondary)), c.ignore_case)
            << c.a << " / " << c.b;
        EXPECT_EQ(sign(en_us().compare(c.a, c.b, CollationStrength::primary)), c.ignore_case_and_non_space)
            << c.a << " / " << c.b;
    }
}

// Cross-check against .NET 10.0.15 on Windows 11 (ICU globalization):
// Enumerable.OrderBy(words, w => w, StringComparer.Create(culture, options)),
// a stable sort, like std::ranges::stable_sort. Literal UTF-8 for readability;
// full-width and kana characters are escaped.
const std::vector<std::string> kCollationWords{
    "Kraków", "krakow", "KRAKOW", "Krakow", "Łódź", "Lodz", "lodz", "co-op", "coop", "co op", "Co-Op", "a", "A",
    "á", "Á", "b", "B", "ab", "a-b", "a b", "a'b", "1st", "10th", "2nd", "Zürich", "Zurich", "zürich", "Ærø",
    "Aero", "Øresund", "Oresund", "ß", "ss", "SS", "Straße", "Strasse", "é", "e", "É", "E", "Ελλάδα", "Москва",
    "ａ", "Ａ", "あ", "ア", "ｱ", "St. Petersburg", "St Petersburg", "Saint Petersburg",
    "(xy)", "xy", "_x", "x_", "Alghero (Sardinia)", "Alghero", "Algeria"};

std::string joined(const std::vector<std::string>& words)
{
    std::string out;
    for (const auto& word : words) {
        out += out.empty() ? "" : " | ";
        out += word;
    }
    return out;
}

TEST(LocaleCollation, Matches_dotnet_current_culture_order)
{
    EXPECT_EQ(joined(sorted(kCollationWords, en_us(), CollationStrength::tertiary)),
              "_x | (xy) | 10th | 1st | 2nd | a | ａ | A | Ａ | á | Á | a b | a-b | a'b | ab | Aero | Ærø | "
              "Algeria | Alghero | Alghero (Sardinia) | b | B | co op | co-op | Co-Op | coop | e | E | é | É | "
              "krakow | Krakow | KRAKOW | Kraków | lodz | Lodz | Łódź | Oresund | Øresund | Saint Petersburg | ss | "
              "SS | ß | St Petersburg | St. Petersburg | Strasse | Straße | x_ | xy | Zurich | zürich | Zürich | "
              "Ελλάδα | Москва | あ | ア | ｱ");
}

TEST(LocaleCollation, Matches_dotnet_ignore_case_order)
{
    EXPECT_EQ(joined(sorted(kCollationWords, en_us(), CollationStrength::secondary)),
              "_x | (xy) | 10th | 1st | 2nd | a | A | á | Á | a b | a-b | a'b | ab | Aero | Ærø | Algeria | "
              "Alghero | Alghero (Sardinia) | ａ | Ａ | b | B | co op | co-op | Co-Op | coop | e | E | é | "
              "É | krakow | KRAKOW | Krakow | Kraków | Lodz | lodz | Łódź | Oresund | Øresund | Saint Petersburg | "
              "ss | SS | ß | St Petersburg | St. Petersburg | Strasse | Straße | x_ | xy | Zurich | Zürich | zürich | "
              "Ελλάδα | Москва | あ | ア | ｱ");
}

TEST(LocaleCollation, Matches_dotnet_ignore_case_and_non_space_order)
{
    EXPECT_EQ(joined(sorted(kCollationWords, en_us(), CollationStrength::primary)),
              "_x | (xy) | 10th | 1st | 2nd | a | A | á | Á | a b | a-b | a'b | ab | Ærø | Aero | Algeria | "
              "Alghero | Alghero (Sardinia) | ａ | Ａ | b | B | co op | co-op | Co-Op | coop | é | e | É | "
              "E | Kraków | krakow | KRAKOW | Krakow | Łódź | Lodz | lodz | Øresund | Oresund | Saint Petersburg | "
              "ß | ss | SS | St Petersburg | St. Petersburg | Straße | Strasse | x_ | xy | Zürich | Zurich | zürich | "
              "Ελλάδα | Москва | あ | ア | ｱ");
}

TEST(LocaleCollation, Matches_dotnet_turkish_order)
{
    const std::vector<std::string> words{"ılık", "Istanbul", "istanbul", "İstanbul", "izmir", "Izmir", "İzmir",
                                         "ırmak", "Irmak", "çay", "cay", "Çay", "dağ", "dag", "öz", "oz",
                                         "şehir", "sehir", "üzüm", "uzum"};
    const std::string expected = "cay | çay | Çay | dag | dağ | ılık | ırmak | Irmak | Istanbul | Izmir | istanbul | "
                                 "İstanbul | izmir | İzmir | oz | öz | sehir | şehir | uzum | üzüm";
    EXPECT_EQ(joined(sorted(words, locale("tr-TR"), CollationStrength::tertiary)), expected);
    EXPECT_EQ(joined(sorted(words, locale("tr-TR"), CollationStrength::secondary)), expected);
}

TEST(LocaleCollation, Turkish_ignore_case_equates_dotless_i_and_capital_i)
{
    const Locale& tr = locale("tr-TR");
    EXPECT_EQ(tr.compare("ı", "I", CollationStrength::secondary), 0);
    EXPECT_NE(tr.compare("i", "I", CollationStrength::secondary), 0);
    EXPECT_EQ(tr.compare("i", "İ", CollationStrength::secondary), 0);
    EXPECT_NE(en_us().compare("ı", "I", CollationStrength::secondary), 0);
}

TEST(LocaleCollation, Concurrent_comparisons_are_safe)
{
    const std::vector<std::string> names{"Tirana", "Budapest", "Kraków", "London Luton", "London Gatwick",
                                         "Alghero (Sardinia)", "Naples", "Barcelona El Prat"};
    const auto expected = sorted(names, en_us(), CollationStrength::secondary);
    std::vector<std::jthread> workers;
    std::vector<int> ok(8, 0);
    for (std::size_t t = 0; t < ok.size(); ++t) {
        workers.emplace_back([&, t] {
            bool all = true;
            for (int round = 0; round < 200; ++round) {
                all = all && sorted(names, en_us(), CollationStrength::secondary) == expected;
                all = all && en_us().starts_with("Milan Bergamo", "milan", CollationStrength::secondary);
            }
            ok[t] = all ? 1 : 0;
        });
    }
    workers.clear(); // join
    EXPECT_EQ(std::ranges::count(ok, 1), static_cast<std::ptrdiff_t>(ok.size()));
}

// ---------------------------------------------------------------------------
// Culture-aware prefix (03 §5 ReachableNames: StartsWith(culture, ignore case))
// ---------------------------------------------------------------------------

TEST(LocaleStartsWith, Ignore_case_prefix)
{
    const auto starts = [](std::string_view text, std::string_view prefix) {
        return en_us().starts_with(text, prefix, CollationStrength::secondary);
    };
    EXPECT_TRUE(starts("Milan Bergamo", "Milan"));
    EXPECT_TRUE(starts("Milan Bergamo", "milan"));
    EXPECT_TRUE(starts("Milan", "MILAN"));
    EXPECT_TRUE(starts("anything", ""));
    EXPECT_FALSE(starts("Bee Two", "Bee One"));
    EXPECT_FALSE(starts("Mil", "Milan"));
    EXPECT_FALSE(starts("Krakow", "Krakó"));      // accents count
    EXPECT_FALSE(starts("öx", "o"));             // a combining mark continues the character
    EXPECT_TRUE(starts("Kraków", "Krakó")); // canonical equivalence
}

TEST(LocaleStartsWith, Case_sensitive_prefix_and_unsupported_strength)
{
    EXPECT_TRUE(en_us().starts_with("Milan Bergamo", "Milan"));
    EXPECT_FALSE(en_us().starts_with("Milan Bergamo", "milan"));
    EXPECT_THROW((void)en_us().starts_with("Milan", "mil", CollationStrength::primary), std::invalid_argument);
}

TEST(LocaleStartsWith, Matches_dotnet_is_prefix_edge_cases)
{
    // CompareInfo.IsPrefix(text, prefix, IgnoreCase / None), recorded with .NET 10 (en-US).
    struct Case {
        std::string_view text;
        std::string_view prefix;
        bool ignore_case;
        bool none;
    };
    const Case cases[] = {
        {"a\0b"sv, "ab", true, true},       // U+0000 is ignorable
        {"ab", "a\0"sv, true, true},
        {"­ab", "ab", true, true},     // soft hyphen is ignorable
        {"ab", "a­", true, true},
        {"æble", "ae", false, false},  // æ is not a+e
        {"Straße", "Strass", false, false},
        {"Straße", "Stras", false, false},
        {"Straße", "STRASSE", false, false},
        {"co-op", "coop", false, false},
        {"coop", "co-op", false, false},
        {"é", "e", false, false},
        {"éx", "é", true, true},
        {"Ab", "a", true, false},
        {"", "a", false, false},
        {"ａb", "a", false, false},
        {"ab", "ａ", false, false},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(en_us().starts_with(c.text, c.prefix, CollationStrength::secondary), c.ignore_case)
            << c.text << " / " << c.prefix;
        EXPECT_EQ(en_us().starts_with(c.text, c.prefix, CollationStrength::tertiary), c.none)
            << c.text << " / " << c.prefix;
    }
}

// ---------------------------------------------------------------------------
// Title case (02 §5.6) and culture casing
// ---------------------------------------------------------------------------

TEST(LocaleTitleCase, CityPlace_names_02_5_6)
{
    // CityPlace: TitleCase_CurrentCulture(ToLowerInvariant(typed)), en-US.
    const std::vector<std::pair<std::string_view, std::string_view>> vectors{
        {"london", "London"},
        {"LONDON", "London"},
        {"frankfurt am main", "Frankfurt Am Main"},
        {"cluj-napoca", "Cluj-Napoca"},
        {"o'hare", "O'hare"},
        {"a’b", "A’B"},
        {"st. petersburg", "St. Petersburg"},
        {"la coruña", "La Coruña"},
        {"x1y", "X1y"},
        {"1st", "1St"},
        {"a_b", "A_B"},
        {"a·b", "A·B"},
        {"(xy)", "(Xy)"},
        {"ǆemal", "ǅemal"},
        {"tenerife (canary islands)", "Tenerife (Canary Islands)"},
    };
    for (const auto& [typed, expected] : vectors) {
        EXPECT_EQ(en_us().title_case(wizz::text::to_lower_invariant(typed)), expected) << "typed: " << typed;
    }
}

TEST(LocaleTitleCase, Turkish_culture_upper_cases_i_with_a_dot)
{
    const Locale& tr = locale("tr-TR");
    EXPECT_EQ(tr.title_case("islands"), "İslands");
    EXPECT_EQ(tr.title_case("tenerife (canary islands)"), "Tenerife (Canary İslands)");
    EXPECT_EQ(en_us().title_case("islands"), "Islands");
}

TEST(LocaleTitleCase, Mixed_case_input_follows_dotnet)
{
    EXPECT_EQ(en_us().title_case("NASA rocket"), "NASA Rocket"); // words without lower case are kept
    EXPECT_EQ(en_us().title_case("mIXed CASE"), "Mixed CASE");
    EXPECT_EQ(en_us().title_case("O'HARE"), "O'hare"); // an apostrophe forces lower case after it
    EXPECT_EQ(en_us().title_case("ǄEMAL"), "ǅEMAL");
    EXPECT_EQ(en_us().title_case("ǉubljana ǌegotin ǳ"), "ǈubljana ǋegotin ǲ");
    EXPECT_EQ(en_us().title_case("éclair café"), "Éclair Café"); // marks continue a word
    EXPECT_EQ(en_us().title_case("\U00010428\U00010428 x"), "\U00010400\U00010428 X");
    EXPECT_EQ(en_us().title_case(""), "");
    EXPECT_EQ(en_us().title_case("  "), "  ");
    EXPECT_EQ(locale("nl-NL").title_case("ijsselmeer"), "IJsselmeer");
    EXPECT_EQ(en_us().title_case("ijsselmeer"), "Ijsselmeer");
    EXPECT_EQ(locale("tr-TR").title_case("ISTANBUL istanbul"), "ISTANBUL İstanbul");
}

TEST(LocaleCasing, Culture_upper_case)
{
    EXPECT_EQ(en_us().to_upper("istanbul"), "ISTANBUL");
    EXPECT_EQ(locale("tr-TR").to_upper("istanbul"), "İSTANBUL");
    EXPECT_EQ(locale("tr-TR").to_upper("ı"), "I");
    EXPECT_EQ(locale("az-Latn-AZ").to_upper("i"), "İ");
    EXPECT_EQ(en_us().to_upper("straße"), "STRAßE"); // simple mapping, no expansion
    EXPECT_EQ(en_us().to_upper("\U00010428"), "\U00010400");
    EXPECT_EQ(locale("de-DE").to_upper("köln"), "KÖLN");
}

} // namespace
