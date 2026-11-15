#include "app_services.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/services/app_paths.hpp"
#include "wizz/services/curl_transport.hpp"
#include "wizz/services/file_cache.hpp"
#include "wizz/ui/replay_transport.hpp"

#include <utility>

namespace wizz::app {

ServiceConfig ServiceConfig::for_run(const std::optional<ui::AutoTestOptions>& autotest)
{
    ServiceConfig config;
    if (!autotest) {
        return config;
    }
    config.data_root = autotest->data_root;
    if (autotest->replay_dir) {
        config.transport = std::make_shared<ui::ReplayTransport>(*autotest->replay_dir);
    }
    if (autotest->fixed_now) {
        config.clock = std::make_shared<FixedClock>(*autotest->fixed_now);  // local zone UTC (D-30)
    }
    if (autotest->no_pacing) {
        config.customize_client = [](WizzClientOptions& client) {
            client.request_spacing = Duration::zero();  // retry back-offs (retry_delays) stay
            client.request_jitter = Duration::zero();
        };
    }
    return config;
}

std::filesystem::path AppServices::settings_path() const
{
    return app_paths::settings_file(data_root);
}

std::shared_ptr<const AppServices> AppServices::create(ServiceConfig config)
{
    auto services = std::make_shared<AppServices>();
    services->data_root = config.data_root;
    services->clock = config.clock ? std::move(config.clock) : std::make_shared<SystemClock>();
    services->locale = config.locale ? std::move(config.locale) : Locale::user_default();
    services->transport = config.transport ? std::move(config.transport) : std::make_shared<http::CurlTransport>();

    const auto cache = [&] {
        return config.use_disk_cache ? FileCache{app_paths::default_cache_dir(config.data_root)} : FileCache::disabled();
    };

    WizzClientOptions client;
    client.clock = services->clock;
    client.locale = services->locale;
    client.cache = cache();
    if (config.customize_client) {
        config.customize_client(client);
    }
    services->wizz = std::make_shared<WizzApiClient>(services->transport, std::move(client));
    services->fx = std::make_shared<CurrencyService>(services->transport, cache(), services->clock);
    services->places = std::make_shared<PlaceResolver>(
        services->transport, cache(), services->clock,
        config.geocoder_delay ? std::move(config.geocoder_delay) : DelayFunc{&cancellable_sleep});
    services->search =
        std::make_shared<FlightSearchService>(services->wizz, services->fx, services->clock, services->locale);
    return services;
}

} // namespace wizz::app
