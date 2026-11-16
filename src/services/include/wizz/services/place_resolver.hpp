#pragma once
// Place resolution (02 §6, §8, §9; protocol-reference §11): turns what the user
// typed into candidate places. Wizz names are matched offline first
// (AirportCatalog::match_place); anything else is geocoded with Photon (komoot),
// falling back to OpenStreetMap Nominatim, and remembered in `geocode.json`.
//
// `wizz::geocoding` holds the pure, stateless pieces (URLs, response parsers,
// JoinParts, Dedupe, ToPlace, WithPinned, the service-failure classification and
// the geocode.json format); `wizz::PlaceResolver` owns the I/O, the memo, the
// Nominatim spacing and the lock that serialises all geocoding.

#include "wizz/core/cancellation.hpp"
#include "wizz/core/catalog.hpp"
#include "wizz/core/domain.hpp"
#include "wizz/core/http.hpp"
#include "wizz/core/progress.hpp"
#include "wizz/core/time.hpp"
#include "wizz/services/file_cache.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wizz::geocoding {

/// 02 §2.13 `PlaceResolver.PlaceDto`: one geocoder hit, also the on-disk cache record.
/// `kind` (the OSM class) is stored and cached but never used by any decision.
struct PlaceDto {
    std::string name;
    std::string detail;  ///< becomes Place::description
    double lat = 0.0;
    double lon = 0.0;
    std::string kind;

    friend bool operator==(const PlaceDto&, const PlaceDto&) = default;
};

/// .NET `KeyNotFoundException` from `JsonElement.GetProperty`: a Photon feature
/// without `properties` makes the whole response unusable (02 §6.6). It is a
/// service failure (02 §6.4).
class MissingPropertyError : public std::runtime_error {
public:
    MissingPropertyError() : std::runtime_error("The given key was not present in the dictionary.") {}
};

/// 02 §6.1 `CacheFile`, in the FileCache directory (%LOCALAPPDATA%\WizzairHelper\cache).
inline constexpr std::string_view kCacheFile = "geocode.json";
/// 02 §6.1 / §6.5: the identifying User-Agent Nominatim's usage policy requires.
inline constexpr std::string_view kUserAgent = "WizzairHelper/1.0 (personal desktop flight finder)";
/// 02 §6.9: the whole file is stale when `now - last write > 90 days` (exactly 90 days is fresh).
inline constexpr Duration kCacheMaxAge = std::chrono::days{90};
/// 02 §6.4: Nominatim call starts are spaced at least 1.1 s apart per resolver.
inline constexpr Duration kNominatimSpacing = std::chrono::milliseconds{1100};
/// 02 §6.5: per request (connect, send and the whole body) and per connection attempt.
inline constexpr std::chrono::milliseconds kRequestTimeout{15'000};
/// 02 §6.2 threshold summary, in km.
inline constexpr double kPinRadiusKm = 150.0;       ///< pin a prefix station to a geocoder hit: <= 150
inline constexpr double kAgreeRadiusKm = 150.0;     ///< geocoder agrees with a typo correction: < 150
inline constexpr double kNamesakeRadiusKm = 300.0;  ///< "has a Wizz airport nearby": <= 300
inline constexpr double kDedupeRadiusKm = 30.0;     ///< same-name hits closer than this are repeats: < 30

/// 02 §6.5 Photon: `https://photon.komoot.io/api/?q={q}&limit=6&lang=en&layer=city`,
/// `{q}` = http::escape_data_string(text) (RFC 3986, upper-case hex, space → %20).
[[nodiscard]] std::string photon_url(std::string_view text);

/// 02 §6.5 Nominatim: `https://nominatim.openstreetmap.org/search?q={q}&format=jsonv2&limit=6&accept-language=en&featureType=settlement`.
[[nodiscard]] std::string nominatim_url(std::string_view text);

/// 02 §6.5: GET `url` with User-Agent kUserAgent, Accept `application/json` and
/// Accept-Language `en` (in that order), no body, 15 s timeout and connect timeout.
/// The transport adds Accept-Encoding itself; no cookies are ever sent.
[[nodiscard]] http::Request geocoder_request(std::string url);

/// 02 §6.6 ParsePhoton (GeoJSON, coordinates are [lon, lat]), then Dedupe.
/// - malformed JSON → json::ParseError;
/// - root, a feature, a present `geometry` or `properties` not an object, or a
///   coordinate that is not a number → json::TypeError (InvalidOperationException);
/// - a coordinate that overflows a double → json::FormatError (not a service failure);
/// - a usable feature without `properties` → MissingPropertyError (the whole response fails).
/// A root without a `features` array gives []. A feature without `geometry`, without
/// `coordinates`, with non-array coordinates or fewer than two is skipped, as is one
/// whose `name` is missing, not a string or white space. The name is not trimmed;
/// kind = osm_value ?? type ?? ""; detail = JoinParts(name, state, country).
[[nodiscard]] std::vector<PlaceDto> parse_photon(std::string_view json);

/// 02 §6.6 ParseNominatim (jsonv2 array whose lat/lon are strings), then Dedupe.
/// Malformed JSON → json::ParseError; an element that is not an object →
/// json::TypeError. A root that is not an array gives []. An element is skipped
/// when lat or lon is not a string that try_parse_double accepts, or when its name
/// (`name`, else the trimmed first comma-separated part of `display_name`) is
/// missing or white space. detail = display_name ?? "", kind = addresstype ?? "".
[[nodiscard]] std::vector<PlaceDto> parse_nominatim(std::string_view json);

/// .NET `double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture)`
/// (verified against .NET 10): optional leading/trailing ASCII white space
/// (U+0009–U+000D, U+0020), an optional sign, ASCII digits with an optional '.'
/// (".5" and "5." are fine), an optional exponent, then optional trailing NULs;
/// correctly rounded, overflow → ±∞, underflow → ±0. When that fails, the
/// Unicode-trimmed text may be "Infinity", "-Infinity", "NaN", "+Infinity",
/// "+NaN" or "-NaN" (ASCII case-insensitive). nullopt for anything else
/// ("47,07", "1,234.5", "0x10", "", "Inf", "∞").
[[nodiscard]] std::optional<double> try_parse_double(std::string_view text);

/// 02 §6.6 JoinParts: drops missing and white-space parts, keeps the first of
/// OrdinalIgnoreCase duplicates and joins the rest with ", ". Parts are not trimmed.
[[nodiscard]] std::string join_parts(std::initializer_list<std::optional<std::string_view>> parts);

/// 02 §6.6 Dedupe: keeps a hit unless an already kept hit has the same name key
/// (TextNormalizer.Key) and lies less than 30 km away (distance_km(kept, hit) < 30).
[[nodiscard]] std::vector<PlaceDto> dedupe(std::span<const PlaceDto> hits);

/// 02 §6.6 ToPlace: {name, detail, (lat, lon), Geocoder, no pins}.
[[nodiscard]] Place to_place(const PlaceDto& dto);

/// 02 §6.3 WithPinned: ToPlace of every hit, in order; a hit gains the codes of
/// `catalog.prefix_matches(text)` (in that order) that lie within 150 km
/// (distance_km(place, station) <= 150). It keeps Source Geocoder, its name and its centre.
[[nodiscard]] std::vector<Place> with_pinned(std::span<const PlaceDto> found, const AirportCatalog& catalog,
                                             std::string_view text);

/// 02 §6.4 IsServiceFailure: false once `stop` was requested (then everything
/// propagates); otherwise true for http::TransportError (network, DNS, TLS,
/// non-2xx status), http::TimeoutError (the 15 s per-request timeout),
/// json::ParseError (JsonException), json::TypeError (InvalidOperationException:
/// wrong JSON shape), MissingPropertyError (KeyNotFoundException) and an
/// OperationCancelled that was not caused by `stop` (.NET's
/// OperationCanceledException clause). Everything else is not a service failure.
[[nodiscard]] bool is_service_failure(const std::exception& error, const std::stop_token& stop) noexcept;

/// One `geocode.json` entry: the memo key (TextNormalizer.Key of the looked-up
/// text) and its deduped hits (at most 6 from the services).
struct GeocodeCacheEntry {
    std::string key;
    std::vector<PlaceDto> hits;

    friend bool operator==(const GeocodeCacheEntry&, const GeocodeCacheEntry&) = default;
};

/// 02 §6.9: reads the `Dictionary<string, List<PlaceDto>>` file. Never throws for
/// bad content:
/// - malformed JSON, the literal null, a root that is not an object → empty;
/// - property names are case-sensitive PascalCase (Name, Detail, Lat, Lon, Kind);
///   missing ones get "" / 0, unknown ones are ignored, null strings read as "";
/// - a property of the wrong kind (a number for Name, a string or null for Lat, a
///   number that overflows a double) or a list element that is neither an object
///   nor null makes the whole file corrupt → empty (JsonException in .NET);
/// - DECISIONS D-23: a value that is null or not an array, or a list containing a
///   null element, is a miss — kept as an empty list, exactly like `[]`;
/// - duplicate keys keep the first key's position and the last value (Dictionary
///   indexer semantics); entry order is the file order.
[[nodiscard]] std::vector<GeocodeCacheEntry> parse_geocode_cache(std::string_view json);

/// 02 §6.9: compact `{"key":[{"Name":…,"Detail":…,"Lat":…,"Lon":…,"Kind":…},…],…}`
/// in entry order; strings escaped like .NET (non-ASCII as \uXXXX), doubles in
/// shortest round-trip form. An entry with a non-finite coordinate (a Nominatim
/// "NaN" / "Infinity") cannot be written as JSON and is left out of the file.
[[nodiscard]] std::string serialize_geocode_cache(std::span<const GeocodeCacheEntry> entries);

} // namespace wizz::geocoding

namespace wizz {

/// 02 §6 PlaceResolver. One instance per app, sharing the app-wide transport.
///
/// Thread-safe: `resolve` and `geocode` may be called from several worker
/// threads at once. All geocoding (memo, disk cache and network) is serialised by
/// one lock per resolver whose wait honours the caller's stop token (02 §6.1);
/// offline matches never take it. Notices are reported synchronously on the
/// calling thread while that lock is held, so a NoticeSink must not block (02 §6.7).
class PlaceResolver {
public:
    using PlaceDto = geocoding::PlaceDto;

    /// Result of one geocode (02 §6.4): the deduped hits, and `failed` = nothing
    /// was found *and* both Photon and Nominatim threw service failures.
    struct GeocodeResult {
        std::vector<PlaceDto> found;
        bool failed = false;
    };

    /// `transport`: the shared HTTP client. `cache`: the geocode.json location
    /// (FileCache::disabled() in tests, FileCache{app_paths::default_cache_dir()}
    /// in the app). `clock`: the 90-day TTL and the Nominatim spacing check (05 §1.1).
    /// `delay`: the Nominatim spacing wait; the real cancellable sleep by default.
    /// Throws std::invalid_argument when `transport`, `clock` or `delay` is empty.
    PlaceResolver(std::shared_ptr<http::Transport> transport, FileCache cache, std::shared_ptr<const Clock> clock,
                  DelayFunc delay = cancellable_sleep);

    PlaceResolver(const PlaceResolver&) = delete;
    PlaceResolver& operator=(const PlaceResolver&) = delete;
    PlaceResolver(PlaceResolver&&) = delete;
    PlaceResolver& operator=(PlaceResolver&&) = delete;
    ~PlaceResolver() = default;

    /// 02 §6.2 ResolveAsync, steps S1..S8:
    /// S1 `catalog.match_place(text)` → [it] (no network, no memo load);
    /// S2 a name key shorter than 2 UTF-16 units → [] (not failed, no network);
    /// S3 geocode(Trim(text));
    /// S4 when Trim(text) prefix-matches stations: the hits pinned to them (within
    ///    150 km) first, then the unpinned hits (LINQ Except: distinct, in order);
    ///    with no pinned hit, [catalog.prefix_place(trimmed)] — never "failed";
    /// S5 candidates = ToPlace of the hits; exactHit = the FIRST hit's name key equals the typed key;
    /// S6 no exactHit and catalog.fuzzy_correct(trimmed) gives a name: the corrected
    ///    place (match_place, else a second geocode of the corrected name — its own
    ///    notices, its failure ignored — taking its first pinned hit, else
    ///    prefix_place) with ` (from “{trimmed}”)` appended to its description,
    ///    followed by the candidates; NeedsConfirmation unless a candidate lies
    ///    within 150 km (strict) of it; CorrectedText = the corrected name;
    /// S7 namesakes: with exactHit and exactly one same-name candidate that has a
    ///    real station within 300 km, that candidate first (others by identity, in
    ///    order) and no confirmation; otherwise the candidates with NeedsConfirmation
    ///    = !exactHit || at least two same-name candidates;
    /// S8 [] with LookupFailed = the first lookup's `failed`.
    /// Throws OperationCancelled when `stop` fires during a wait or a request; other
    /// exceptions that are not service failures (or any after a cancel) propagate.
    [[nodiscard]] PlaceLookup resolve(std::string_view text, const AirportCatalog& catalog, const NoticeSink& notices,
                                      std::stop_token stop);

    /// 02 §6.4 GeocodeAsync(text): under the resolver lock (cancellable), loads the
    /// memo from geocode.json on first use (90-day TTL on the whole file), returns a
    /// non-empty memo entry for Key(text) without notice or network, otherwise
    /// reports `Looking up "{text}" (Photon geocoder)` and asks Photon; only when
    /// Photon gave nothing (failed or no usable hit) it reports
    /// `Looking up "{text}" (OpenStreetMap Nominatim)` and asks Nominatim (spaced
    /// 1.1 s after the previous Nominatim call). A non-empty answer is stored under
    /// Key(text) and the whole memo is rewritten to geocode.json; empty answers are
    /// never remembered. `text` is used as given (callers pass trimmed text).
    [[nodiscard]] GeocodeResult geocode(std::string_view text, const NoticeSink& notices, std::stop_token stop);

    /// Callers blocked on the geocoding lock right now (tests wait for a caller to queue
    /// instead of sleeping).
    [[nodiscard]] int lock_waiters() const;

private:
    /// RAII ownership of the geocoding lock (SemaphoreSlim(1,1) with a cancellable wait).
    class GeocodingLock;

    void load_memo();
    [[nodiscard]] const std::vector<PlaceDto>* memo_find(const std::string& key) const;
    void memo_store(const std::string& key, std::vector<PlaceDto> hits);

    [[nodiscard]] std::vector<PlaceDto> ask_photon(std::string_view text, const std::stop_token& stop) const;
    [[nodiscard]] std::vector<PlaceDto> ask_nominatim(std::string_view text, const std::stop_token& stop);
    void wait_for_nominatim_slot(const std::stop_token& stop);
    [[nodiscard]] std::string get_text(std::string url, const std::stop_token& stop) const;

    std::shared_ptr<http::Transport> transport_;
    FileCache cache_;
    std::shared_ptr<const Clock> clock_;
    DelayFunc delay_;

    // The geocoding lock.
    mutable std::mutex mutex_;
    std::condition_variable_any released_;
    bool busy_ = false;
    int waiters_ = 0;  ///< guarded by mutex_

    // Guarded by the geocoding lock (only touched while busy_ is owned).
    bool memo_loaded_ = false;
    std::vector<geocoding::GeocodeCacheEntry> memo_;          ///< insertion order (written back in this order)
    std::unordered_map<std::string, std::size_t> memo_index_; ///< key → index into memo_ (ordinal)
    std::optional<UtcInstant> last_nominatim_call_;           ///< nullopt = never (the first call never waits)
};

} // namespace wizz
