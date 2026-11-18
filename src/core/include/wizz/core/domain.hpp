#pragma once
// Shared domain vocabulary (02 §2, 03 §3, §4.1, §5.1, §7.1, §10.1). Data only:
// behaviour lives in catalog.hpp, planner.hpp, pricing.hpp, result_views.hpp,
// booking_links.hpp and round_trips.hpp. Values are immutable once built;
// results are shared as std::shared_ptr<const T> so the UI can keep identity.

#include "wizz/core/decimal.hpp"
#include "wizz/core/geo.hpp"
#include "wizz/core/time.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wizz {

/// One route out of a station (02 §2.4). `next_departure` is the naive local
/// date-time parsed from `operationStartDate`; nullopt = unknown (never skips).
struct RouteConnection {
    std::string iata;                            ///< trimmed, upper-cased; may be a group / pseudo code
    std::optional<LocalDateTime> next_departure;

    friend bool operator==(const RouteConnection&, const RouteConnection&) = default;
};

/// One Wizz station from asset/map, including "(All Airports)" pseudo stations (02 §2.3).
struct Airport {
    std::string iata;           ///< trimmed, upper-cased, never empty
    std::string name;           ///< trimmed shortName, or the trimmed (not upper-cased) iata
    std::string country_name;
    std::string country_code;
    std::string currency_code;  ///< upper-cased; currency of departures from here
    GeoPoint location;          ///< (0,0) when the map has no numeric coordinates
    std::optional<std::string> mac;   ///< upper-cased group code; nullopt for stand-alone stations
    bool is_fake_station = false;
    bool has_city_group = false;      ///< derived by the catalog (mac names a group with a pseudo station)
    std::vector<RouteConnection> connections;  ///< map order

    [[nodiscard]] const std::string& group_key() const noexcept { return mac ? *mac : iata; }
    [[nodiscard]] std::string display() const { return name + " (" + iata + ")"; }
};
using AirportPtr = std::shared_ptr<const Airport>;

enum class PriceType { regular, wizz_discount_club };
enum class FareStatus { available, check_on_site };
enum class PlaceSource { wizz_airport, wizz_city, geocoder };

/// Price in a currency (02 §2.7). Equality is value equality (decimal ignores scale).
struct Money {
    Decimal amount;
    std::string currency;  ///< upper-cased

    friend bool operator==(const Money&, const Money&) = default;
};

/// One timetable row: the cheapest fare of one day on one route (02 §2.8, 04 §9.4).
struct DayFare {
    std::string from;                       ///< departureStation, upper-cased (response row, not the query)
    std::string to;                         ///< arrivalStation, upper-cased
    Date date;
    LocalDateTime cheapest_departure;       ///< naive airport-local wall clock
    std::vector<LocalDateTime> departures;  ///< sorted ascending, may mix arrival siblings
    Money price_per_person;                 ///< amount 0 when check_on_site
    FareStatus status = FareStatus::available;
    bool has_mac_flight = false;

    /// Field-wise; the price compares by value (Money ignores the decimal's scale).
    friend bool operator==(const DayFare&, const DayFare&) = default;
};

/// A resolved location (02 §2.10).
struct Place {
    std::string name;
    std::string description;
    GeoPoint location;
    PlaceSource source = PlaceSource::wizz_airport;
    std::vector<std::string> pinned_airports;  ///< codes that always belong to the area

    friend bool operator==(const Place&, const Place&) = default;
};

/// Resolver answer (02 §2.12).
struct PlaceLookup {
    std::vector<Place> candidates;
    bool needs_confirmation = false;
    std::optional<std::string> corrected_text;
    bool lookup_failed = false;
};

/// What the user asked for, one leg (03 §3).
struct LegSearchRequest {
    Place origin;
    double origin_radius_km = 200;
    std::optional<Place> destination;  ///< nullopt = "anywhere"
    double destination_radius_km = 100;
    Date from;
    Date to;
    int adults = 1;
    PriceType price_type = PriceType::regular;
    std::string currency = "EUR";      ///< any case; results store the upper-case form
    std::string label = "Search";
};

/// The cheapest fare of one day on one route, enriched (02 §2.11, 03 §6.1).
struct FlightResult {
    DayFare fare;
    AirportPtr from_airport;
    AirportPtr to_airport;
    double from_distance_km = 0;
    std::optional<double> to_distance_km;  ///< nullopt for "anywhere"
    bool outside_destination_radius = false;
    int adults = 1;
    std::string target_currency;           ///< upper-cased display currency
    std::optional<Decimal> price_per_person_converted;
    std::optional<Decimal> total_converted;
    std::string booking_url;

    [[nodiscard]] Date date() const noexcept { return fare.date; }
    [[nodiscard]] bool is_available() const noexcept { return fare.status == FareStatus::available; }
};
using FlightResultPtr = std::shared_ptr<const FlightResult>;

/// An area airport with its distance from the place centre.
struct AreaAirport {
    AirportPtr airport;
    double distance_km = 0;
};

/// One timetableV2 call (03 §4.1).
struct PlannedRequest {
    AirportPtr from;        ///< real departure station
    std::string query_to;   ///< arrivalStation sent to Wizz
    std::string group_key;  ///< arrival group covered (Mac ?? Iata)
    Date start_date;
    Date end_date;
};

/// Output of the pure planner (03 §4.1).
struct SearchPlan {
    std::vector<AreaAirport> origin_airports;
    std::vector<AreaAirport> destination_airports;
    std::vector<PlannedRequest> requests;
    Date effective_from;
    Date effective_to;
    int routes_considered = 0;
    int routes_skipped_not_yet_flying = 0;  ///< not-yet-flying + suspended
    std::vector<std::string> notes;         ///< blocking notes first, then info notes
    bool same_area = false;
};

/// Units per 1 EUR (03 §7.1). Keys are upper-cased codes; may contain "EUR".
struct FxRates {
    std::map<std::string, Decimal> per_eur;
    Date date;
    std::string source;
    bool is_approximate = false;
};
using FxRatesPtr = std::shared_ptr<const FxRates>;

/// Result of one leg search (03 §5.1).
struct LegSearchResult {
    LegSearchRequest request;
    SearchPlan plan;
    std::vector<FlightResultPtr> flights;   ///< SortByPrice order
    std::vector<std::string> warnings;
    FxRatesPtr rates;
    int requests_completed = 0;
    int routes_not_served = 0;
    bool cancelled = false;
    std::optional<std::string> abort_reason;
    Duration elapsed{};
    /// Planned requests answered from fares fetched earlier without sending anything (D-70).
    int requests_from_cache = 0;
    /// Planned requests that asked Wizz Air only for the days not fetched yet (D-70).
    int requests_partly_cached = 0;
    /// How old the oldest reused fare was when the search ended; nullopt when nothing was reused.
    std::optional<Duration> oldest_reused_age;
};
using LegSearchResultPtr = std::shared_ptr<const LegSearchResult>;

/// A paired outbound + return (03 §10.1).
struct RoundTrip {
    FlightResultPtr outbound;
    FlightResultPtr inbound;
    int nights = 0;
    std::optional<Decimal> total;
    bool same_home_airport = false;
    bool same_away_airport = false;
    std::optional<std::string> combined_booking_url;
};

/// MatchLegs answer (03 §10.2).
struct LegMatch {
    std::vector<FlightResultPtr> outbound;
    std::vector<FlightResultPtr> inbound;
    std::optional<std::string> problem;
};

} // namespace wizz
