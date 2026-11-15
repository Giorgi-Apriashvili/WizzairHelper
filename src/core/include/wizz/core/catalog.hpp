#pragma once
// AirportCatalog: every Wizz station of the route map plus the offline place
// matching that turns typed text into a Place without any network (02 §5).
//
// The catalog is immutable after construction and safe for concurrent const
// use. It is shared as `std::shared_ptr<const AirportCatalog>`; its stations are
// `AirportPtr`s whose identity stays stable for the catalog's lifetime (02 §2.3:
// code that needs the catalog's instance must call `find`, never keep an
// Airport it passed in). Everything derived from names — the name keys of the
// stations, the pseudo-station city names and the typo-correction index of
// 02 §5.8 — is computed eagerly in the constructor, so no lazy state exists.
//
// Collation (the RealAirports order, SuggestionNames) and title case
// (CityPlace names) go through the injected Locale (07 §1); tests use en-US.
// Code lookups are ordinal and case-insensitive (07 §1). Name comparisons use
// TextNormalizer keys (02 §4) compared ordinally, with lengths counted in
// UTF-16 code units.

#include "wizz/core/domain.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/core/text.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wizz {

class AirportCatalog;
using AirportCatalogPtr = std::shared_ptr<const AirportCatalog>;

/// One entry of the typo-correction name index (02 §5.8 NameIndex).
struct NameIndexEntry {
    std::u16string key;   ///< TextNormalizer key of the name or of its first word (UTF-16)
    std::string display;  ///< what FuzzyCorrect returns: a full name, a city name or a first word

    friend bool operator==(const NameIndexEntry&, const NameIndexEntry&) = default;
};

class AirportCatalog {
public:
    /// 02 §5.2. De-duplicates `airports` by IATA (OrdinalIgnoreCase): the first
    /// occurrence keeps its position, the last occurrence supplies every field
    /// (DECISIONS D-10, 07 §4, CT-LATENT-02). Derives `has_city_group` (true when
    /// `mac` names the IATA of a pseudo station; an input that is already true
    /// stays true) and sorts the real stations by name with a stable,
    /// culture-aware (tertiary) collation of `locale`, so ties keep map order.
    /// Throws std::invalid_argument when `locale` is null.
    AirportCatalog(std::vector<Airport> airports, std::shared_ptr<const Locale> locale);

    /// 02 §5.1 FromMapJson: parses the `asset/map` body `{"cities":[...]}`.
    /// - malformed JSON → json::ParseError;
    /// - root, a `cities` element or a `connections` element not an object →
    ///   json::TypeError (InvalidOperationException);
    /// - no `cities` array → FormatError("Wizz route map JSON has no 'cities' array.");
    /// - no usable station → FormatError("Wizz route map contains no stations.");
    /// - a numeric latitude/longitude that overflows a double → json::FormatError;
    /// - a null `locale` → std::invalid_argument (from the constructor).
    /// Callers that fall back to a cached map (02 §5.1) catch json::Error and FormatError.
    /// Field rules: strict kind tests (`Str` = string only, `Num` = number only,
    /// else 0; `isFakeStation` only for the literal `true`), Unicode trim,
    /// invariant upper-casing of codes, `operationStartDate` parsed with
    /// parse_local_datetime when it is a JSON string (D-22), a missing or blank
    /// `shortName` falls back to the trimmed (not upper-cased) `iata`.
    [[nodiscard]] static AirportCatalogPtr from_map_json(std::string_view json, std::shared_ptr<const Locale> locale);

    /// Every station in map order, pseudo stations included (02 §5.2 `All`).
    [[nodiscard]] std::span<const AirportPtr> all() const noexcept { return all_; }
    /// Real stations sorted by name (culture collation, stable) — `RealAirports`.
    [[nodiscard]] std::span<const AirportPtr> real_airports() const noexcept { return real_; }
    /// The locale used for collation and title case.
    [[nodiscard]] const std::shared_ptr<const Locale>& locale() const noexcept { return locale_; }

    /// 02 §5.3 Find: the station whose IATA equals `code.Trim()` (OrdinalIgnoreCase),
    /// pseudo stations included; nullptr when unknown or empty.
    [[nodiscard]] AirportPtr find(std::string_view code) const;

    /// 02 §5.3 GroupMembers: real stations whose GroupKey equals `group_key`
    /// (OrdinalIgnoreCase) in RealAirports order; if there are none and
    /// `find(group_key)` is a real station, just that station. Never pseudo stations.
    [[nodiscard]] std::vector<AirportPtr> group_members(std::string_view group_key) const;

    /// 02 §5.3 Near: real stations with distance_km(point, location) <= radius_km
    /// (inclusive), stable-sorted by distance (ties keep RealAirports order). A
    /// negative or NaN radius gives an empty list; +∞ or DBL_MAX gives every real
    /// station. Named `airports_near` because <windows.h> defines `near` as a macro.
    [[nodiscard]] std::vector<AreaAirport> airports_near(GeoPoint point, double radius_km) const;

    /// 02 §5.3 SuggestionNames: the autocomplete list — each station's name (the
    /// city name for pseudo stations) in a set ordered by the locale's
    /// case-insensitive, accent-sensitive collation (SortedSet with
    /// CurrentCultureIgnoreCase). Case-insensitive duplicates collapse to the
    /// first spelling inserted in map order.
    [[nodiscard]] std::vector<std::string> suggestion_names() const;

    /// 02 §5.4 CityNameOfFake: removes a trailing "(All Airports)" or "Any" as
    /// `Regex.Replace(name, @"\s*(\(\s*all airports\s*\)|\bany)\s*$", "", IgnoreCase)`
    /// would, then trims. Hand-written: `\s` is char.IsWhiteSpace, `\b` uses the
    /// .NET word characters (L*, Mn, Nd, Pc, U+200C, U+200D) per UTF-16 unit, and
    /// letters match ASCII case-insensitively ('i' also matches U+0130, as under en-US).
    [[nodiscard]] static std::string city_name_of_fake(std::string_view name);

    /// 02 §5.5 MatchPlace: the offline match, rules R1..R7 in order —
    /// R1 trailing "(XXX)" naming a known station (Name = the whole trimmed text);
    /// R2 exactly one real station with the same name key; R3 the first pseudo
    /// station (map order) whose city-name key matches; R4 a 3-unit key that is a
    /// code; R5 several real stations with the same name key (CityPlace);
    /// R6 whole-word prefix matches that all share one group with a pseudo station
    /// (CityPlace); R7 nullopt. Empty or white-space text gives nullopt.
    [[nodiscard]] std::optional<Place> match_place(std::string_view text) const;

    /// 02 §5.6 PlaceFor. Pseudo station: Name `display_name`, Description
    /// "{display_name}, {country} — Wizz Air airports: {member codes}", WizzCity,
    /// pinned = group_members codes. Real station: Description
    /// "{airport.name} ({iata}), {country}", WizzAirport, pinned [iata]. The
    /// location is the station's own position.
    [[nodiscard]] Place place_for(const Airport& airport, std::string_view display_name) const;

    /// 02 §5.6 CityPlace over non-null `airports` (given order): Name = locale
    /// title case of the invariant-lower-cased `typed`; location = the pseudo
    /// station of their single shared non-null mac if one exists, else the
    /// centroid; Description "{name}, {airports[0].country} — Wizz Air airports:
    /// {codes}"; WizzCity; pinned = the codes. Throws std::invalid_argument
    /// ("No points given.") for an empty list.
    [[nodiscard]] Place city_place(std::span<const AirportPtr> airports, std::string_view typed) const;

    /// 02 §5.7 PrefixMatches: real stations (RealAirports order) whose name key is
    /// longer than Key(text), starts with it (ordinal) and continues with a
    /// character that is not a letter or digit. Keys shorter than 3 UTF-16 units
    /// give an empty list.
    [[nodiscard]] std::vector<AirportPtr> prefix_matches(std::string_view text) const;

    /// 02 §5.7 PrefixPlace: CityPlace(prefix_matches(text), Trim(text)), or nullopt.
    [[nodiscard]] std::optional<Place> prefix_place(std::string_view text) const;

    /// 02 §5.8 FuzzyCorrect: the unique closest Wizz name within edit distance
    /// 1 (keys up to 7 UTF-16 units) or 2 (longer keys), excluding exact matches;
    /// nullopt for keys shorter than 4 units, for no candidate, or when two
    /// different displays (OrdinalIgnoreCase) tie at the best distance.
    [[nodiscard]] std::optional<std::string> fuzzy_correct(std::string_view text) const;

    /// 02 §5.8 NameIndex: for each station in map order, (Key(name), name) and,
    /// when the key's first word (split on ' ', '/', '(', '-') has at least 4
    /// units, (first word of the key, first word of the original name);
    /// distinct (key, display) pairs, first kept, order preserved.
    [[nodiscard]] std::span<const NameIndexEntry> name_index() const noexcept { return name_index_; }

    /// 02 §5.8 EditDistance: optimal string alignment distance (Levenshtein plus
    /// one adjacent transposition per substring) over UTF-16 code units, ordinal.
    [[nodiscard]] static int edit_distance(std::u16string_view a, std::u16string_view b);

private:
    /// A pseudo station with its city name and that name's key (R3, CityPlace).
    struct FakeCity {
        AirportPtr airport;
        std::string city_name;
        std::u16string key;
    };

    [[nodiscard]] static std::vector<NameIndexEntry> build_name_index(std::span<const AirportPtr> all);
    /// First pseudo station in map order whose IATA equals `iata` (ordinal), or nullptr.
    [[nodiscard]] const Airport* fake_with_iata(std::string_view iata) const noexcept;

    std::shared_ptr<const Locale> locale_;
    std::vector<AirportPtr> all_;
    std::vector<AirportPtr> real_;
    std::vector<std::u16string> real_keys_;  ///< Key(name) of real_[i]
    std::vector<FakeCity> fake_cities_;      ///< pseudo stations, map order
    std::unordered_map<std::string, AirportPtr, text::OrdinalIgnoreCaseHash, text::OrdinalIgnoreCaseEqual> by_iata_;
    std::vector<NameIndexEntry> name_index_;
};

} // namespace wizz
