// PlaceResolver (02 §6.1, §6.2, §6.4, §6.5, §6.7, §6.9): the S1..S8 decision tree,
// the memoised Photon → Nominatim lookup, the Nominatim spacing and the geocoding lock.

#include "wizz/services/place_resolver.hpp"

#include "wizz/core/geo.hpp"
#include "wizz/core/text.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace wizz {
namespace {

void report(const NoticeSink& notices, std::string message)
{
    if (notices) {
        notices(ClientNotice{.message = std::move(message), .wait_until_utc = std::nullopt});
    }
}

[[nodiscard]] std::u16string name_key(std::string_view name)
{
    return text::normalize_key(text::utf8_to_utf16(name));
}

[[nodiscard]] bool is_same_double(double a, double b) noexcept
{
    return a == b || (std::isnan(a) && std::isnan(b));
}

/// The record equality LINQ `Except` applies to two unpinned places (02 §6.2 S4
/// details): Name, Description and Source equal, and the location equal with
/// double.Equals semantics (NaN equals NaN). Unpinned places share one empty pin list.
[[nodiscard]] bool is_same_unpinned_place(const Place& a, const Place& b) noexcept
{
    return a.name == b.name && a.description == b.description && a.source == b.source &&
           is_same_double(a.location.latitude, b.location.latitude) &&
           is_same_double(a.location.longitude, b.location.longitude);
}

[[nodiscard]] PlaceLookup lookup(std::vector<Place> candidates, bool needs_confirmation,
                                 std::optional<std::string> corrected_text = std::nullopt, bool lookup_failed = false)
{
    return PlaceLookup{.candidates = std::move(candidates),
                       .needs_confirmation = needs_confirmation,
                       .corrected_text = std::move(corrected_text),
                       .lookup_failed = lookup_failed};
}

} // namespace

// ---------------------------------------------------------------------------
// The geocoding lock: SemaphoreSlim(1, 1) with WaitAsync(ct)
// ---------------------------------------------------------------------------

class PlaceResolver::GeocodingLock {
public:
    GeocodingLock(PlaceResolver& owner, const std::stop_token& stop) : owner_(owner)
    {
        throw_if_cancelled(stop);  // WaitAsync observes a cancelled token before taking a free slot
        std::unique_lock lock(owner_.mutex_);
        if (!owner_.released_.wait(lock, stop, [this] { return !owner_.busy_; })) {
            throw OperationCancelled{};
        }
        owner_.busy_ = true;
    }

    ~GeocodingLock()
    {
        {
            const std::scoped_lock lock(owner_.mutex_);
            owner_.busy_ = false;
        }
        owner_.released_.notify_all();
    }

    GeocodingLock(const GeocodingLock&) = delete;
    GeocodingLock& operator=(const GeocodingLock&) = delete;

private:
    PlaceResolver& owner_;
};

// ---------------------------------------------------------------------------

PlaceResolver::PlaceResolver(std::shared_ptr<http::Transport> transport, FileCache cache,
                             std::shared_ptr<const Clock> clock, DelayFunc delay)
    : transport_(std::move(transport)), cache_(std::move(cache)), clock_(std::move(clock)), delay_(std::move(delay))
{
    if (!transport_) {
        throw std::invalid_argument("PlaceResolver needs an HTTP transport.");
    }
    if (!clock_) {
        throw std::invalid_argument("PlaceResolver needs a clock.");
    }
    if (!delay_) {
        throw std::invalid_argument("PlaceResolver needs a delay function.");
    }
}

PlaceLookup PlaceResolver::resolve(std::string_view text, const AirportCatalog& catalog, const NoticeSink& notices,
                                   std::stop_token stop)
{
    // S1: a Wizz name, matched offline — no network, no memo load.
    if (std::optional<Place> wizz = catalog.match_place(text)) {
        std::vector<Place> only;
        only.push_back(std::move(*wizz));
        return lookup(std::move(only), false);
    }

    // S2: too short to look up ("x"); not a failure.
    const std::u16string key = name_key(text);
    if (key.size() < 2) {
        return lookup({}, false);
    }
    const std::string trimmed{text::trim(text)};

    // S3
    const GeocodeResult first = geocode(trimmed, notices, stop);

    // S4: a station is named after the place ("Barcelona El Prat", "Frankfurt Hahn").
    if (!catalog.prefix_matches(trimmed).empty()) {
        std::vector<Place> places = geocoding::with_pinned(first.found, catalog, trimmed);
        std::vector<Place> result;
        for (const Place& place : places) {
            if (!place.pinned_airports.empty()) {
                result.push_back(place);
            }
        }
        if (result.empty()) {
            std::vector<Place> fallback;
            fallback.push_back(*catalog.prefix_place(trimmed));  // never empty: prefix_matches is not
            return lookup(std::move(fallback), false);
        }
        // near ++ places.Except(near): the unpinned places in geocoder order, distinct.
        const auto near_count = static_cast<std::ptrdiff_t>(result.size());
        for (Place& place : places) {
            if (!place.pinned_airports.empty()) {
                continue;
            }
            const bool repeated = std::any_of(result.begin() + near_count, result.end(),
                                              [&](const Place& kept) { return is_same_unpinned_place(kept, place); });
            if (!repeated) {
                result.push_back(std::move(place));
            }
        }
        return lookup(std::move(result), false);
    }

    // S5
    std::vector<Place> candidates;
    candidates.reserve(first.found.size());
    for (const PlaceDto& dto : first.found) {
        candidates.push_back(geocoding::to_place(dto));
    }
    std::vector<std::u16string> candidate_keys;
    candidate_keys.reserve(candidates.size());
    for (const Place& candidate : candidates) {
        candidate_keys.push_back(name_key(candidate.name));
    }
    const bool exact_hit = !candidates.empty() && candidate_keys.front() == key;  // only the first hit counts

    // S6: a likely typo of a Wizz name ("Londn" → London) is suggested, never applied silently.
    if (!exact_hit) {
        if (const std::optional<std::string> corrected = catalog.fuzzy_correct(trimmed)) {
            std::optional<Place> fixed = catalog.match_place(*corrected);
            if (!fixed) {
                const GeocodeResult second = geocode(*corrected, notices, stop);  // its "failed" is ignored
                for (Place& place : geocoding::with_pinned(second.found, catalog, *corrected)) {
                    if (!place.pinned_airports.empty()) {
                        fixed = std::move(place);
                        break;
                    }
                }
                if (!fixed) {
                    fixed = catalog.prefix_place(*corrected);
                }
            }
            if (fixed) {
                fixed->description += " (from \u201C" + trimmed + "\u201D)";
                const GeoPoint at = fixed->location;
                const bool agrees = std::ranges::any_of(candidates, [&](const Place& candidate) {
                    return distance_km(candidate.location, at) < geocoding::kAgreeRadiusKm;
                });
                std::vector<Place> result;
                result.reserve(candidates.size() + 1);
                result.push_back(std::move(*fixed));
                std::ranges::move(candidates, std::back_inserter(result));
                return lookup(std::move(result), !agrees, *corrected);
            }
            // fixed == null: practically unreachable; fall through to S7 (02 §6.2).
        }
    }

    // S7: namesakes (Vienna, Austria / Vienna, Virginia) — the one with a Wizz airport nearby wins.
    if (!candidates.empty()) {
        std::size_t same_name = 0;
        std::vector<std::size_t> with_wizz;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (candidate_keys[i] != key) {
                continue;
            }
            ++same_name;
            if (!catalog.airports_near(candidates[i].location, geocoding::kNamesakeRadiusKm).empty()) {
                with_wizz.push_back(i);
            }
        }
        if (exact_hit && with_wizz.size() == 1) {
            const std::size_t winner = with_wizz.front();
            std::vector<Place> result;
            result.reserve(candidates.size());
            result.push_back(std::move(candidates[winner]));
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                if (i != winner) {  // "not the same object": identity, not value
                    result.push_back(std::move(candidates[i]));
                }
            }
            return lookup(std::move(result), false);
        }
        return lookup(std::move(candidates), !exact_hit || same_name >= 2);
    }

    // S8
    return lookup({}, false, std::nullopt, first.failed);
}

PlaceResolver::GeocodeResult PlaceResolver::geocode(std::string_view text, const NoticeSink& notices,
                                                    std::stop_token stop)
{
    const std::string key = text::normalize_key(text);
    const GeocodingLock lock(*this, stop);

    if (!memo_loaded_) {
        load_memo();
    }
    if (const std::vector<PlaceDto>* const cached = memo_find(key); cached != nullptr && !cached->empty()) {
        return GeocodeResult{.found = *cached, .failed = false};
    }

    std::vector<PlaceDto> found;
    bool photon_failed = false;
    bool nominatim_failed = false;

    report(notices, std::format("Looking up \"{}\" (Photon geocoder)", text));
    try {
        found = ask_photon(text, stop);
    } catch (const std::exception& error) {
        if (!geocoding::is_service_failure(error, stop)) {
            throw;
        }
        photon_failed = true;
    }

    if (found.empty()) {
        report(notices, std::format("Looking up \"{}\" (OpenStreetMap Nominatim)", text));
        try {
            found = ask_nominatim(text, stop);
        } catch (const std::exception& error) {
            if (!geocoding::is_service_failure(error, stop)) {
                throw;
            }
            nominatim_failed = true;
        }
    }

    if (found.empty()) {
        return GeocodeResult{.found = {}, .failed = photon_failed && nominatim_failed};
    }
    memo_store(key, found);
    cache_.try_write(geocoding::kCacheFile, geocoding::serialize_geocode_cache(memo_));
    return GeocodeResult{.found = std::move(found), .failed = false};
}

void PlaceResolver::load_memo()
{
    memo_.clear();
    memo_index_.clear();
    if (const std::optional<std::string> file = cache_.try_read(geocoding::kCacheFile, geocoding::kCacheMaxAge, *clock_)) {
        memo_ = geocoding::parse_geocode_cache(*file);
        for (std::size_t i = 0; i < memo_.size(); ++i) {
            memo_index_.emplace(memo_[i].key, i);
        }
    }
    memo_loaded_ = true;
}

const std::vector<PlaceResolver::PlaceDto>* PlaceResolver::memo_find(const std::string& key) const
{
    const auto found = memo_index_.find(key);
    return found == memo_index_.end() ? nullptr : &memo_[found->second].hits;
}

void PlaceResolver::memo_store(const std::string& key, std::vector<PlaceDto> hits)
{
    if (const auto found = memo_index_.find(key); found != memo_index_.end()) {
        memo_[found->second].hits = std::move(hits);
        return;
    }
    memo_index_.emplace(key, memo_.size());
    memo_.push_back(geocoding::GeocodeCacheEntry{.key = key, .hits = std::move(hits)});
}

std::vector<PlaceResolver::PlaceDto> PlaceResolver::ask_photon(std::string_view text, const std::stop_token& stop) const
{
    return geocoding::parse_photon(get_text(geocoding::photon_url(text), stop));
}

std::vector<PlaceResolver::PlaceDto> PlaceResolver::ask_nominatim(std::string_view text, const std::stop_token& stop)
{
    wait_for_nominatim_slot(stop);
    return geocoding::parse_nominatim(get_text(geocoding::nominatim_url(text), stop));
}

void PlaceResolver::wait_for_nominatim_slot(const std::stop_token& stop)
{
    if (last_nominatim_call_) {
        const Duration since = clock_->utc_now() - *last_nominatim_call_;
        if (since < geocoding::kNominatimSpacing) {
            // A wall clock that jumped backwards would ask for more than the spacing;
            // waiting the full 1.1 s already guarantees it.
            delay_(std::min(geocoding::kNominatimSpacing - since, geocoding::kNominatimSpacing), stop);
        }
    }
    last_nominatim_call_ = clock_->utc_now();  // after the wait, before sending
}

std::string PlaceResolver::get_text(std::string url, const std::stop_token& stop) const
{
    const http::Response response = transport_->send(geocoding::geocoder_request(std::move(url)), stop);
    if (response.status < 200 || response.status > 299) {
        // HttpResponseMessage.EnsureSuccessStatusCode → HttpRequestException (02 §6.5)
        throw http::TransportError(
            std::format("Response status code does not indicate success: {}.", response.status));
    }
    return http::decode_body_text(response);
}

} // namespace wizz
