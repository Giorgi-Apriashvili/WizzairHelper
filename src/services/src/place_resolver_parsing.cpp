// The stateless half of the place resolver (02 §6.3, §6.4, §6.5, §6.6, §6.9):
// geocoder URLs and requests, the Photon / Nominatim parsers, JoinParts, Dedupe,
// ToPlace, WithPinned, the service-failure classification and the geocode.json format.

#include "wizz/services/place_resolver.hpp"

#include "wizz/core/geo.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/text.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <expected>
#include <format>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace wizz::geocoding {
namespace {

// ---------------------------------------------------------------------------
// double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture)
// ---------------------------------------------------------------------------

/// Number.Parsing's IsWhite: the only white space NumberStyles.AllowLeading/TrailingWhite accept.
[[nodiscard]] constexpr bool is_number_white(char c) noexcept
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/// The exponent saturates here, far beyond any double; the result is then ±∞ or ±0 anyway.
constexpr std::int64_t kExponentLimit = 1'000'000;

/// The numeric grammar of NumberStyles.Float: [ws][sign]digits[.digits][e[sign]digits][ws][NUL…],
/// at least one mantissa digit; an exponent marker without digits is not part of the number.
[[nodiscard]] std::optional<double> parse_float_number(std::string_view s)
{
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n && is_number_white(s[i])) {
        ++i;
    }
    bool negative = false;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        ++i;
    }
    const std::size_t int_begin = i;
    while (i < n && is_ascii_digit(s[i])) {
        ++i;
    }
    const std::string_view int_digits = s.substr(int_begin, i - int_begin);
    std::string_view frac_digits;
    if (i < n && s[i] == '.') {
        ++i;
        const std::size_t frac_begin = i;
        while (i < n && is_ascii_digit(s[i])) {
            ++i;
        }
        frac_digits = s.substr(frac_begin, i - frac_begin);
    }
    if (int_digits.empty() && frac_digits.empty()) {
        return std::nullopt;
    }
    std::int64_t exponent = 0;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        std::size_t j = i + 1;
        bool negative_exponent = false;
        if (j < n && (s[j] == '+' || s[j] == '-')) {
            negative_exponent = s[j] == '-';
            ++j;
        }
        if (j < n && is_ascii_digit(s[j])) {
            while (j < n && is_ascii_digit(s[j])) {
                exponent = std::min(exponent * 10 + (s[j] - '0'), kExponentLimit);
                ++j;
            }
            if (negative_exponent) {
                exponent = -exponent;
            }
            i = j;
        }
        // Otherwise the 'e' is not consumed and the text fails below, as in .NET.
    }
    while (i < n && is_number_white(s[i])) {
        ++i;
    }
    while (i < n && s[i] == '\0') {
        ++i;
    }
    if (i != n) {
        return std::nullopt;
    }

    // Canonical text for the correctly rounded conversion: [-]int.frac e exp.
    std::string canonical;
    canonical.reserve(int_digits.size() + frac_digits.size() + 16);
    if (negative) {
        canonical += '-';
    }
    canonical += int_digits.empty() ? std::string_view{"0"} : int_digits;
    canonical += '.';
    canonical += frac_digits.empty() ? std::string_view{"0"} : frac_digits;
    canonical += 'e';
    canonical += std::to_string(exponent);

    double value = 0.0;
    const char* const first = canonical.data();
    const char* const last = first + canonical.size();
    const auto [end, error] = std::from_chars(first, last, value, std::chars_format::general);
    if (error == std::errc{} && end == last) {
        return value;
    }
    if (error != std::errc::result_out_of_range) {
        return std::nullopt;  // cannot happen for the canonical form
    }
    // Out of range: decide between overflow (±∞) and underflow (±0) from the
    // decimal exponent of the first significant digit (.NET Core 3.0+ semantics).
    std::int64_t first_significant = -1;
    std::int64_t position = 0;
    for (const std::string_view part : {int_digits, frac_digits}) {
        for (const char c : part) {
            if (c != '0' && first_significant < 0) {
                first_significant = position;
            }
            ++position;
        }
    }
    const double magnitude = [&] {
        if (first_significant < 0) {
            return 0.0;
        }
        const std::int64_t scientific =
            static_cast<std::int64_t>(int_digits.size()) - first_significant - 1 + exponent;
        return scientific > 0 ? std::numeric_limits<double>::infinity() : 0.0;
    }();
    return negative ? -magnitude : magnitude;
}

/// The named constants .NET accepts after the numeric grammar failed: the Unicode-trimmed
/// text compared OrdinalIgnoreCase with the invariant symbols.
[[nodiscard]] std::optional<double> parse_float_symbol(std::string_view s) noexcept
{
    constexpr double kInfinity = std::numeric_limits<double>::infinity();
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    const std::string_view t = text::trim(s);
    const auto is = [](std::string_view a, std::string_view b) { return text::ordinal_ignore_case_equals(a, b); };
    if (is(t, "Infinity")) {
        return kInfinity;
    }
    if (is(t, "-Infinity")) {
        return -kInfinity;
    }
    if (is(t, "NaN")) {
        return kNaN;
    }
    if (t.starts_with('+')) {
        const std::string_view rest = t.substr(1);
        if (is(rest, "Infinity")) {
            return kInfinity;
        }
        if (is(rest, "NaN")) {
            return kNaN;
        }
        return std::nullopt;
    }
    if (t.starts_with('-') && is(t.substr(1), "NaN")) {
        return kNaN;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// geocode.json
// ---------------------------------------------------------------------------

/// A JsonException while deserialising Dictionary<string, List<PlaceDto>>: the whole file is unusable.
struct CorruptCache {};

template <typename T>
using CacheRead = std::expected<T, CorruptCache>;

/// A string property: JSON null reads as "" (a null .NET string), any other non-string kind is corrupt.
[[nodiscard]] CacheRead<std::string> read_string_property(const json::Value& value)
{
    if (value.is_null()) {
        return std::string{};
    }
    if (!value.is_string()) {
        return std::unexpected(CorruptCache{});
    }
    return std::string{value.as_string()};
}

/// A double property: anything but a number that fits a double is corrupt (null included).
[[nodiscard]] CacheRead<double> read_double_property(const json::Value& value)
{
    if (!value.is_number()) {
        return std::unexpected(CorruptCache{});
    }
    try {
        return value.as_double();
    } catch (const json::Error&) {
        return std::unexpected(CorruptCache{});
    }
}

/// One list element: an object → the record (case-sensitive names, last duplicate
/// wins, unknown names ignored); null → nullopt (a D-23 miss); any other kind → corrupt.
[[nodiscard]] CacheRead<std::optional<PlaceDto>> read_cached_dto(const json::Value& element)
{
    if (element.is_null()) {
        return std::optional<PlaceDto>{};
    }
    if (!element.is_object()) {
        return std::unexpected(CorruptCache{});
    }
    PlaceDto dto;
    for (const json::Member& member : element.as_object()) {
        if (member.name == "Name" || member.name == "Detail" || member.name == "Kind") {
            auto text = read_string_property(member.value);
            if (!text) {
                return std::unexpected(text.error());
            }
            std::string& target = member.name == "Name" ? dto.name : member.name == "Detail" ? dto.detail : dto.kind;
            target = std::move(*text);
        } else if (member.name == "Lat" || member.name == "Lon") {
            const auto number = read_double_property(member.value);
            if (!number) {
                return std::unexpected(number.error());
            }
            (member.name == "Lat" ? dto.lat : dto.lon) = *number;
        }
    }
    return std::optional<PlaceDto>{std::move(dto)};
}

/// One dictionary value. Not an array (null included) or containing a null element:
/// a miss, kept as an empty list (D-23). Every element is still validated, because a
/// malformed one later in the list makes .NET reject the whole file.
[[nodiscard]] CacheRead<std::vector<PlaceDto>> read_cached_hits(const json::Value& value)
{
    if (!value.is_array()) {
        return std::vector<PlaceDto>{};
    }
    std::vector<PlaceDto> hits;
    bool miss = false;
    for (const json::Value& element : value.as_array()) {
        auto dto = read_cached_dto(element);
        if (!dto) {
            return std::unexpected(dto.error());
        }
        if (!dto->has_value()) {
            miss = true;
        } else if (!miss) {
            hits.push_back(std::move(**dto));
        }
    }
    if (miss) {
        hits.clear();
    }
    return hits;
}

[[nodiscard]] bool is_writable(const PlaceDto& dto) noexcept
{
    return std::isfinite(dto.lat) && std::isfinite(dto.lon);
}

} // namespace

std::string photon_url(std::string_view text)
{
    return std::format("https://photon.komoot.io/api/?q={}&limit=6&lang=en&layer=city", http::escape_data_string(text));
}

std::string nominatim_url(std::string_view text)
{
    return std::format(
        "https://nominatim.openstreetmap.org/search?q={}&format=jsonv2&limit=6&accept-language=en&featureType=settlement",
        http::escape_data_string(text));
}

http::Request geocoder_request(std::string url)
{
    http::Request request;
    request.method = "GET";
    request.url = std::move(url);
    request.headers = {
        {"User-Agent", std::string{kUserAgent}},
        {"Accept", "application/json"},
        {"Accept-Language", "en"},
    };
    request.timeout = kRequestTimeout;
    request.connect_timeout = kRequestTimeout;
    return request;
}

std::optional<double> try_parse_double(std::string_view text)
{
    if (const std::optional<double> number = parse_float_number(text)) {
        return number;
    }
    return parse_float_symbol(text);
}

std::string join_parts(std::initializer_list<std::optional<std::string_view>> parts)
{
    std::vector<std::string_view> kept;
    kept.reserve(parts.size());
    for (const std::optional<std::string_view>& part : parts) {
        if (!part || text::is_null_or_white_space(*part)) {
            continue;
        }
        const bool repeated = std::ranges::any_of(
            kept, [&](std::string_view earlier) { return text::ordinal_ignore_case_equals(earlier, *part); });
        if (!repeated) {
            kept.push_back(*part);
        }
    }
    std::string joined;
    for (const std::string_view part : kept) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += part;
    }
    return joined;
}

std::vector<PlaceDto> dedupe(std::span<const PlaceDto> hits)
{
    std::vector<PlaceDto> result;
    std::vector<std::u16string> result_keys;
    for (const PlaceDto& hit : hits) {
        std::u16string key = text::normalize_key(text::utf8_to_utf16(hit.name));
        const GeoPoint point{hit.lat, hit.lon};
        bool repeat = false;
        for (std::size_t i = 0; i < result.size() && !repeat; ++i) {
            repeat = result_keys[i] == key &&
                     distance_km(GeoPoint{result[i].lat, result[i].lon}, point) < kDedupeRadiusKm;
        }
        if (!repeat) {
            result.push_back(hit);
            result_keys.push_back(std::move(key));
        }
    }
    return result;
}

Place to_place(const PlaceDto& dto)
{
    return Place{
        .name = dto.name,
        .description = dto.detail,
        .location = GeoPoint{dto.lat, dto.lon},
        .source = PlaceSource::geocoder,
        .pinned_airports = {},
    };
}

std::vector<Place> with_pinned(std::span<const PlaceDto> found, const AirportCatalog& catalog, std::string_view text)
{
    const std::vector<AirportPtr> prefixed = catalog.prefix_matches(text);
    std::vector<Place> places;
    places.reserve(found.size());
    for (const PlaceDto& dto : found) {
        Place place = to_place(dto);
        for (const AirportPtr& airport : prefixed) {
            if (distance_km(place.location, airport->location) <= kPinRadiusKm) {
                place.pinned_airports.push_back(airport->iata);
            }
        }
        places.push_back(std::move(place));
    }
    return places;
}

std::vector<PlaceDto> parse_photon(std::string_view json)
{
    const json::Value root = json::parse(json);
    const json::Value* const features = root.find("features");  // root not an object → TypeError
    if (features == nullptr || !features->is_array()) {
        return {};
    }
    std::vector<PlaceDto> list;
    for (const json::Value& feature : features->as_array()) {
        const json::Value* const geometry = feature.find("geometry");  // feature not an object → TypeError
        if (geometry == nullptr) {
            continue;
        }
        const json::Value* const coordinates = geometry->find("coordinates");
        if (coordinates == nullptr || !coordinates->is_array() || coordinates->as_array().size() < 2) {
            continue;
        }
        const json::Value* const properties = feature.find("properties");
        if (properties == nullptr) {
            throw MissingPropertyError{};  // f.GetProperty("properties"): the whole response fails
        }
        const std::optional<std::string_view> name = properties->get_string("name");
        if (!name || text::is_null_or_white_space(*name)) {
            continue;
        }
        std::optional<std::string_view> kind = properties->get_string("osm_value");
        if (!kind) {
            kind = properties->get_string("type");
        }
        const std::span<const json::Value> point = coordinates->as_array();
        PlaceDto dto;
        dto.name = std::string{*name};
        dto.detail = join_parts({*name, properties->get_string("state"), properties->get_string("country")});
        dto.lat = point[1].as_double();  // GeoJSON order is [lon, lat]
        dto.lon = point[0].as_double();
        dto.kind = std::string{kind.value_or(std::string_view{})};
        list.push_back(std::move(dto));
    }
    return dedupe(list);
}

std::vector<PlaceDto> parse_nominatim(std::string_view json)
{
    const json::Value root = json::parse(json);
    if (!root.is_array()) {
        return {};
    }
    std::vector<PlaceDto> list;
    for (const json::Value& element : root.as_array()) {
        const std::optional<std::string_view> lat_text = element.get_string("lat");  // not an object → TypeError
        const std::optional<double> lat = lat_text ? try_parse_double(*lat_text) : std::nullopt;
        if (!lat) {
            continue;
        }
        const std::optional<std::string_view> lon_text = element.get_string("lon");
        const std::optional<double> lon = lon_text ? try_parse_double(*lon_text) : std::nullopt;
        if (!lon) {
            continue;
        }
        const std::string_view display = element.get_string("display_name").value_or(std::string_view{});
        std::string_view name = element.get_string("name").value_or(std::string_view{});
        if (text::is_null_or_white_space(name)) {
            name = text::trim(display.substr(0, display.find(',')));
        }
        if (text::is_null_or_white_space(name)) {
            continue;
        }
        PlaceDto dto;
        dto.name = std::string{name};
        dto.detail = std::string{display};
        dto.lat = *lat;
        dto.lon = *lon;
        dto.kind = std::string{element.get_string("addresstype").value_or(std::string_view{})};
        list.push_back(std::move(dto));
    }
    return dedupe(list);
}

bool is_service_failure(const std::exception& error, const std::stop_token& stop) noexcept
{
    if (stop.stop_requested()) {
        return false;
    }
    return dynamic_cast<const http::TransportError*>(&error) != nullptr ||
           dynamic_cast<const http::TimeoutError*>(&error) != nullptr ||
           dynamic_cast<const json::ParseError*>(&error) != nullptr ||
           dynamic_cast<const json::TypeError*>(&error) != nullptr ||
           dynamic_cast<const MissingPropertyError*>(&error) != nullptr ||
           dynamic_cast<const OperationCancelled*>(&error) != nullptr;
}

std::vector<GeocodeCacheEntry> parse_geocode_cache(std::string_view json)
{
    json::Value root;
    try {
        root = json::parse(json);
    } catch (const json::Error&) {
        return {};
    }
    if (!root.is_object()) {
        return {};  // the literal null, or a root of the wrong kind
    }
    std::vector<GeocodeCacheEntry> entries;
    std::unordered_map<std::string, std::size_t> index;
    for (const json::Member& member : root.as_object()) {
        auto hits = read_cached_hits(member.value);
        if (!hits) {
            return {};
        }
        if (const auto found = index.find(member.name); found != index.end()) {
            entries[found->second].hits = std::move(*hits);  // dictionary[key] = value: position kept
        } else {
            index.emplace(member.name, entries.size());
            entries.push_back(GeocodeCacheEntry{.key = member.name, .hits = std::move(*hits)});
        }
    }
    return entries;
}

std::string serialize_geocode_cache(std::span<const GeocodeCacheEntry> entries)
{
    json::Writer writer;
    writer.begin_object();
    for (const GeocodeCacheEntry& entry : entries) {
        if (!std::ranges::all_of(entry.hits, is_writable)) {
            continue;
        }
        writer.key(entry.key).begin_array();
        for (const PlaceDto& dto : entry.hits) {
            writer.begin_object()
                .key("Name")
                .string(dto.name)
                .key("Detail")
                .string(dto.detail)
                .key("Lat")
                .number(dto.lat)
                .key("Lon")
                .number(dto.lon)
                .key("Kind")
                .string(dto.kind)
                .end_object();
        }
        writer.end_array();
    }
    writer.end_object();
    return writer.take();
}

} // namespace wizz::geocoding

