// WizzApiClient: construction, API base URL (04 §7.1, §7.2, §7.5) and the
// route map (04 §8). Timetables: wizz_client_timetable.cpp; the SendApi state
// machine: wizz_client_send.cpp.

#include "wizz/services/wizz_client.hpp"

#include "wizz/core/errors.hpp"
#include "wizz/core/json.hpp"
#include "wizz/core/text.hpp"
#include "wizz/services/wizz_protocol.hpp"

#include <format>
#include <stdexcept>
#include <utility>

namespace wizz {

namespace {

// 04 §3.1 notice texts (load-bearing: the UI log keys on "API version" and "cached").
constexpr std::string_view kLookingUpNotice = "Looking up the current Wizz Air API version";
constexpr std::string_view kCachedMapNotice = "Wizz Air route map unavailable — using the cached copy";

constexpr std::string_view kDiscoveryFailure =
    "Could not find the Wizz Air API address on wizzair.com. The site may be down, blocking this network, or "
    "showing a bot check. Details: ";
constexpr std::string_view kMapUnreadable = "The Wizz Air route map could not be read: ";

constexpr std::string_view kMapPath = "asset/map?languageCode=en-gb";

/// Discovery GET headers (04 §2.3): no Origin or Referer.
[[nodiscard]] http::Headers discovery_headers()
{
    return {
        {"User-Agent", std::string{kBrowserUserAgent}},
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
        {"Accept-Language", "en-GB,en;q=0.9"},
    };
}

} // namespace

WizzApiClient::WizzApiClient(std::shared_ptr<http::Transport> transport, WizzClientOptions options)
    : transport_(std::move(transport)),
      options_(std::move(options)),
      gate_(options_.request_spacing, options_.request_jitter, options_.clock, options_.delay),
      reuse_window_ticks_(options_.timetable_cache_age.count())
{
    if (!transport_) {
        throw std::invalid_argument("WizzApiClient needs an HTTP transport.");
    }
    if (!options_.locale) {
        throw std::invalid_argument("WizzApiClient needs a locale for the route map.");
    }
}

void WizzApiClient::notify(const NoticeSink& notices, std::string message, std::optional<UtcInstant> wait_until_utc)
{
    if (notices) {
        notices(ClientNotice{.message = std::move(message), .wait_until_utc = wait_until_utc});
    }
}

std::chrono::milliseconds WizzApiClient::attempt_timeout() const
{
    return std::chrono::ceil<std::chrono::milliseconds>(options_.request_timeout);
}

std::optional<std::string> WizzApiClient::current_api_url() const
{
    if (const std::shared_ptr<const std::string> url = api_url_.load()) {
        return *url;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 04 §7.1 GetApiUrl
// ---------------------------------------------------------------------------

std::string WizzApiClient::get_api_url(const NoticeSink& notices, std::stop_token stop)
{
    if (const std::shared_ptr<const std::string> url = api_url_.load()) {
        return *url;
    }
    const CancellableLock discovery{discovery_lock_, stop};
    if (const std::shared_ptr<const std::string> url = api_url_.load()) {
        return *url;  // another caller finished discovery meanwhile (P-33)
    }

    if (const std::optional<std::string> cached =
            options_.cache.try_read(kApiUrlCacheFile, std::nullopt, *options_.clock)) {
        const std::string_view trimmed = text::trim(*cached);
        if (protocol::is_valid_cached_api_url(trimmed)) {
            auto url = std::make_shared<const std::string>(trimmed);
            api_url_.store(url);  // not re-written to disk
            return *url;
        }
        // A value failing the strict pattern is ignored (not deleted); discovery overwrites it.
    }

    auto url = std::make_shared<const std::string>(discover(notices, stop));
    options_.cache.try_write(kApiUrlCacheFile, *url);  // exact text, no newline (CT-API-14)
    api_url_.store(url);
    return *url;
}

// ---------------------------------------------------------------------------
// 04 §7.2 Discover
// ---------------------------------------------------------------------------

std::string WizzApiClient::discover(const NoticeSink& notices, std::stop_token stop)
{
    std::vector<std::string> problems;
    for (const std::string& page : options_.discovery_pages) {
        notify(notices, std::string{kLookingUpNotice});
        try {
            gate_.wait_turn(stop);
            const http::Request request{
                .method = "GET",
                .url = page,
                .headers = discovery_headers(),
                .body = std::nullopt,
                .content_type = std::nullopt,
                .timeout = attempt_timeout(),  // starts after the gate wait
            };
            request_count_.fetch_add(1);  // before sending
            const http::Response response = transport_->send(request, stop);
            // The status is not checked: error pages that embed the config count (04 §7.2).
            if (std::optional<std::string> url = protocol::extract_api_url(http::decode_body_text(response))) {
                return std::move(*url);
            }
            problems.push_back(std::format("{}: HTTP {}, no apiUrl in page", page, response.status));
        } catch (const http::TimeoutError&) {
            throw_if_cancelled(stop);  // a user cancel wins over the timeout
            problems.push_back(std::format("{}: timed out", page));
        } catch (const http::TransportError& error) {
            throw_if_cancelled(stop);
            problems.push_back(std::format("{}: {}", page, error.what()));
        }
        // OperationCancelled and every other error propagate.
    }

    std::string details;
    for (const std::string& problem : problems) {
        if (!details.empty()) {
            details += "; ";
        }
        details += problem;
    }
    throw WizzApiError(std::string{kDiscoveryFailure} + details);
}

// ---------------------------------------------------------------------------
// 04 §7.5 InvalidateApiUrl (no lock: only values equal to `stale` are cleared)
// ---------------------------------------------------------------------------

void WizzApiClient::invalidate_api_url(const std::string& stale)
{
    std::shared_ptr<const std::string> current = api_url_.load();
    if (current && *current == stale) {
        // Leaves a newer URL stored by another caller meanwhile untouched.
        (void)api_url_.compare_exchange_strong(current, nullptr);
    }
    if (const std::optional<std::string> cached =
            options_.cache.try_read(kApiUrlCacheFile, std::nullopt, *options_.clock);
        cached && text::trim(*cached) == stale) {
        options_.cache.try_delete(kApiUrlCacheFile);
    }
}

// ---------------------------------------------------------------------------
// 04 §8 GetAirports
// ---------------------------------------------------------------------------

AirportCatalogPtr WizzApiClient::try_parse_map(const std::optional<std::string>& json) const
{
    if (!json) {
        return nullptr;
    }
    try {
        return AirportCatalog::from_map_json(*json, options_.locale);
    } catch (const json::Error&) {
        return nullptr;  // a corrupt map.json counts as absent
    } catch (const FormatError&) {
        return nullptr;
    }
}

AirportCatalogPtr WizzApiClient::stale_map(const NoticeSink& notices)
{
    AirportCatalogPtr stale = try_parse_map(options_.cache.try_read(kMapCacheFile, std::nullopt, *options_.clock));
    if (stale) {
        notify(notices, std::string{kCachedMapNotice});
        catalog_.store(stale);
    }
    return stale;
}

AirportCatalogPtr WizzApiClient::get_airports(const NoticeSink& notices, std::stop_token stop, bool force_refresh)
{
    if (!force_refresh) {
        if (AirportCatalogPtr catalog = catalog_.load()) {
            return catalog;
        }
    }
    const CancellableLock map_guard{map_lock_, stop};
    if (!force_refresh) {
        if (AirportCatalogPtr catalog = catalog_.load()) {
            return catalog;
        }
        // Warm start: no request and no API-URL lookup, so current_api_url() stays empty.
        if (AirportCatalogPtr fresh =
                try_parse_map(options_.cache.try_read(kMapCacheFile, options_.map_max_age, *options_.clock))) {
            catalog_.store(fresh);
            return fresh;
        }
    }

    std::string unreadable;  // the parse failure, when that is what went wrong
    try {
        const std::string json = send_api("GET", kMapPath, std::nullopt, notices, stop);
        AirportCatalogPtr catalog = AirportCatalog::from_map_json(json, options_.locale);
        options_.cache.try_write(kMapCacheFile, json);  // only after a successful parse
        catalog_.store(catalog);
        return catalog;
    } catch (const WizzApiError&) {
        // Blocked and validation errors included; OperationCancelled is not caught.
        if (AirportCatalogPtr stale = stale_map(notices)) {
            return stale;
        }
        throw;  // unchanged dynamic type, message and status
    } catch (const json::Error& error) {
        unreadable = error.what();
    } catch (const FormatError& error) {
        unreadable = error.what();
    }
    if (AirportCatalogPtr stale = stale_map(notices)) {
        return stale;
    }
    throw WizzApiError(std::string{kMapUnreadable} + unreadable);
}

} // namespace wizz
