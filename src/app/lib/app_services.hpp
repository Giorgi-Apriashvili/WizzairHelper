#pragma once
// The services one app instance owns (06 §3 "one HTTP client and one request
// gate per application instance"): a single transport shared by the Wizz
// client, the exchange-rate service and the geocoder, the clock, the locale
// service and the data root that holds settings.json, errors.log and cache\.
//
// Normal runs use libcurl, the system clock, the Windows user locale (07 §1)
// and %LOCALAPPDATA%\WizzairHelper. The hidden --autotest companions of
// DECISIONS D-30 swap in a replay transport (--replay), a fixed clock
// (--fixed-now) and another data root (--data-root). Tests build their own
// configuration (in-memory replay rules, no pacing, disabled caches).

#include "wizz/core/clocks.hpp"
#include "wizz/core/http.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/services/currency_service.hpp"
#include "wizz/services/flight_search_service.hpp"
#include "wizz/services/place_resolver.hpp"
#include "wizz/services/wizz_client.hpp"
#include "wizz/ui/autotest_options.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace wizz::app {

struct ServiceConfig {
    /// --data-root (D-30); nullopt = %LOCALAPPDATA%\WizzairHelper.
    std::optional<std::filesystem::path> data_root;
    /// The one HTTP transport; null = CurlTransport (Windows proxy settings).
    std::shared_ptr<http::Transport> transport;
    /// null = SystemClock.
    std::shared_ptr<const Clock> clock;
    /// null = the Windows user locale (Locale::user_default()).
    std::shared_ptr<const Locale> locale;
    /// false = every FileCache disabled (tests).
    bool use_disk_cache = true;
    /// Keep the reused fares in `cache\fares.json` (D-67); false keeps them in memory only.
    bool fare_cache_on_disk = true;
    /// Adjusts the Wizz client before it is built (tests: no pacing, instant delays).
    std::function<void(WizzClientOptions&)> customize_client;
    /// The geocoder's delay function (Nominatim spacing); empty = cancellable_sleep.
    DelayFunc geocoder_delay;

    /// The configuration of a normal run, or of an --autotest run with its D-30
    /// companions: ReplayTransport for --replay (throws ui::ReplayConfigError for a bad
    /// directory), FixedClock (local zone UTC) for --fixed-now, the --data-root, no
    /// request spacing or jitter for --no-pacing. An --autotest run keeps fares.json only
    /// in its --data-root profile, never in the user's (D-67).
    [[nodiscard]] static ServiceConfig for_run(const std::optional<ui::AutoTestOptions>& autotest);
};

struct AppServices {
    std::shared_ptr<const Clock> clock;
    std::shared_ptr<const Locale> locale;
    std::shared_ptr<http::Transport> transport;
    std::shared_ptr<WizzApiClient> wizz;
    std::shared_ptr<CurrencyService> fx;
    std::shared_ptr<PlaceResolver> places;
    std::shared_ptr<FlightSearchService> search;
    /// The root of settings.json / errors.log / cache\ (nullopt = the default root).
    std::optional<std::filesystem::path> data_root;

    /// `<root>\settings.json` (01 §13).
    [[nodiscard]] std::filesystem::path settings_path() const;

    /// Builds the services of `config`.
    [[nodiscard]] static std::shared_ptr<const AppServices> create(ServiceConfig config);
};

} // namespace wizz::app
