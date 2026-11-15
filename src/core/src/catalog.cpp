// AirportCatalog construction, map parsing and lookups (02 §5.1–§5.3).

#include "wizz/core/catalog.hpp"

#include "wizz/core/datetime_parse.hpp"
#include "wizz/core/errors.hpp"
#include "wizz/core/json.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace wizz {

namespace {

using text::OrdinalIgnoreCaseEqual;
using text::OrdinalIgnoreCaseHash;

/// `Str(e, n)` of 02 §5.1 followed by `Trim`: nullopt when the member is missing
/// or not a JSON string. Throws json::TypeError when `e` is not an object.
[[nodiscard]] std::optional<std::string_view> trimmed_str(const json::Value& e, std::string_view name)
{
    const std::optional<std::string_view> value = e.get_string(name);
    if (!value) {
        return std::nullopt;
    }
    return text::trim(*value);
}

/// `Num(e, n)` of 02 §5.1: the number when the member is a JSON number, else 0.
[[nodiscard]] double num(const json::Value& e, std::string_view name)
{
    const json::Value* value = e.find(name);
    return value != nullptr && value->is_number() ? value->as_double() : 0.0;
}

[[nodiscard]] std::vector<RouteConnection> parse_connections(const json::Value& city)
{
    std::vector<RouteConnection> connections;
    const json::Value* list = city.find("connections");
    if (list == nullptr || !list->is_array()) {
        return connections;
    }
    for (const json::Value& x : list->as_array()) {
        const std::optional<std::string_view> to = x.get_string("iata");
        if (!to || text::is_null_or_white_space(*to)) {
            continue;
        }
        std::optional<LocalDateTime> next;
        if (const json::Value* date = x.find("operationStartDate"); date != nullptr && date->is_string()) {
            next = parse_local_datetime(date->as_string());
        }
        connections.push_back(RouteConnection{text::to_upper_invariant(text::trim(*to)), next});
    }
    return connections;
}

[[nodiscard]] std::optional<Airport> parse_station(const json::Value& city)
{
    const std::optional<std::string_view> iata = city.get_string("iata");  // TypeError for a non-object
    if (!iata || text::is_null_or_white_space(*iata)) {
        return std::nullopt;
    }
    std::vector<RouteConnection> connections = parse_connections(city);

    const std::string_view trimmed_iata = text::trim(*iata);
    const std::optional<std::string_view> name = trimmed_str(city, "shortName");
    const std::optional<std::string_view> mac = city.get_string("mac");

    Airport airport;
    airport.iata = text::to_upper_invariant(trimmed_iata);
    airport.name = std::string{name && !name->empty() ? *name : trimmed_iata};
    airport.country_name = std::string{trimmed_str(city, "countryName").value_or(std::string_view{})};
    airport.country_code = std::string{trimmed_str(city, "countryCode").value_or(std::string_view{})};
    airport.currency_code = text::to_upper_invariant(trimmed_str(city, "currencyCode").value_or(std::string_view{}));
    airport.location = GeoPoint{num(city, "latitude"), num(city, "longitude")};
    if (mac && !text::is_null_or_white_space(*mac)) {
        airport.mac = text::to_upper_invariant(text::trim(*mac));
    }
    const json::Value* fake = city.find("isFakeStation");
    airport.is_fake_station = fake != nullptr && fake->is_true();
    airport.connections = std::move(connections);
    return airport;
}

} // namespace

AirportCatalogPtr AirportCatalog::from_map_json(std::string_view json, std::shared_ptr<const Locale> locale)
{
    const json::Value root = json::parse(json);
    const json::Value* cities = root.find("cities");  // TypeError when the root is not an object
    if (cities == nullptr || !cities->is_array()) {
        throw FormatError("Wizz route map JSON has no 'cities' array.");
    }

    std::vector<Airport> list;
    for (const json::Value& city : cities->as_array()) {
        if (std::optional<Airport> airport = parse_station(city)) {
            list.push_back(std::move(*airport));
        }
    }
    if (list.empty()) {
        throw FormatError("Wizz route map contains no stations.");
    }
    return std::make_shared<const AirportCatalog>(std::move(list), std::move(locale));
}

AirportCatalog::AirportCatalog(std::vector<Airport> airports, std::shared_ptr<const Locale> locale)
    : locale_(std::move(locale))
{
    if (!locale_) {
        throw std::invalid_argument("AirportCatalog requires a locale.");
    }

    // D-10: one entry per IATA (OrdinalIgnoreCase) at the first position, holding the last value.
    std::vector<Airport> list;
    list.reserve(airports.size());
    std::unordered_map<std::string, std::size_t, OrdinalIgnoreCaseHash, OrdinalIgnoreCaseEqual> position;
    for (Airport& airport : airports) {
        if (const auto it = position.find(airport.iata); it != position.end()) {
            list[it->second] = std::move(airport);
        } else {
            position.emplace(airport.iata, list.size());
            list.push_back(std::move(airport));
        }
    }

    std::unordered_set<std::string, OrdinalIgnoreCaseHash, OrdinalIgnoreCaseEqual> city_groups;
    for (const Airport& airport : list) {
        if (airport.is_fake_station) {
            city_groups.insert(airport.iata);
        }
    }

    all_.reserve(list.size());
    for (Airport& airport : list) {
        if (airport.mac && !airport.has_city_group && city_groups.contains(*airport.mac)) {
            airport.has_city_group = true;
        }
        all_.push_back(std::make_shared<const Airport>(std::move(airport)));
    }

    by_iata_.reserve(all_.size());
    for (const AirportPtr& airport : all_) {
        by_iata_.emplace(airport->iata, airport);  // unique after the de-duplication
        if (!airport->is_fake_station) {
            real_.push_back(airport);
        }
    }
    std::ranges::stable_sort(real_, CollationLess{*locale_, CollationStrength::tertiary},
                             [](const AirportPtr& airport) -> const std::string& { return airport->name; });

    real_keys_.reserve(real_.size());
    for (const AirportPtr& airport : real_) {
        real_keys_.push_back(text::normalize_key(text::utf8_to_utf16(airport->name)));
    }
    for (const AirportPtr& airport : all_) {
        if (airport->is_fake_station) {
            std::string city = city_name_of_fake(airport->name);
            std::u16string key = text::normalize_key(text::utf8_to_utf16(city));
            fake_cities_.push_back(FakeCity{airport, std::move(city), std::move(key)});
        }
    }
    name_index_ = build_name_index(all_);
}

AirportPtr AirportCatalog::find(std::string_view code) const
{
    const auto it = by_iata_.find(text::trim(code));
    return it == by_iata_.end() ? nullptr : it->second;
}

std::vector<AirportPtr> AirportCatalog::group_members(std::string_view group_key) const
{
    std::vector<AirportPtr> members;
    for (const AirportPtr& airport : real_) {
        if (text::ordinal_ignore_case_equals(airport->group_key(), group_key)) {
            members.push_back(airport);
        }
    }
    if (members.empty()) {
        if (AirportPtr self = find(group_key); self && !self->is_fake_station) {
            members.push_back(std::move(self));
        }
    }
    return members;
}

std::vector<AreaAirport> AirportCatalog::airports_near(GeoPoint point, double radius_km) const
{
    std::vector<AreaAirport> result;
    for (const AirportPtr& airport : real_) {
        const double distance = distance_km(point, airport->location);
        if (distance <= radius_km) {  // false for NaN on either side
            result.push_back(AreaAirport{airport, distance});
        }
    }
    std::ranges::stable_sort(result, std::ranges::less{}, &AreaAirport::distance_km);
    return result;
}

std::vector<std::string> AirportCatalog::suggestion_names() const
{
    std::set<std::string, CollationLess> names{CollationLess{*locale_, CollationStrength::secondary}};
    for (const AirportPtr& airport : all_) {
        names.insert(airport->is_fake_station ? city_name_of_fake(airport->name) : airport->name);  // first spelling kept
    }
    return {names.begin(), names.end()};
}

const Airport* AirportCatalog::fake_with_iata(std::string_view iata) const noexcept
{
    const auto it = std::ranges::find_if(fake_cities_, [iata](const FakeCity& fake) { return fake.airport->iata == iata; });
    return it == fake_cities_.end() ? nullptr : it->airport.get();
}

} // namespace wizz
