// ServiceConfig::for_run (D-30): a normal run uses the defaults; --autotest with its
// companions swaps in the replay transport, the fixed clock, the data root and, for
// --no-pacing, a client without request spacing (retry back-offs unchanged). fares.json
// is kept in a normal run and in an --autotest run's --data-root profile only (D-67).

#include "app_services.hpp"
#include "app_test_support.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/services/app_paths.hpp"
#include "wizz/services/wizz_client.hpp"
#include "wizz/ui/replay_transport.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace {

using wizz::app::ServiceConfig;
using wizz::test::app::replay_root;

[[nodiscard]] wizz::ui::AutoTestOptions options_with_replay()
{
    wizz::ui::AutoTestOptions options;
    options.output_dir = std::filesystem::temp_directory_path();
    options.replay_dir = replay_root() / "named_destination";
    options.data_root = std::filesystem::temp_directory_path() / "wizz-profile";
    options.fixed_now = wizz::FixedClock::kDefaultUtcNow;
    return options;
}

TEST(ServiceConfig, A_normal_run_uses_the_defaults)
{
    const ServiceConfig config = ServiceConfig::for_run(std::nullopt);
    EXPECT_FALSE(config.transport);
    EXPECT_FALSE(config.clock);
    EXPECT_FALSE(config.data_root);
    EXPECT_FALSE(config.customize_client);
    EXPECT_TRUE(config.use_disk_cache);
    EXPECT_TRUE(config.fare_cache_on_disk);
}

TEST(ServiceConfig, D_30_Replay_fixed_clock_and_data_root)
{
    const wizz::ui::AutoTestOptions options = options_with_replay();
    const ServiceConfig config = ServiceConfig::for_run(options);
    EXPECT_NE(std::dynamic_pointer_cast<wizz::ui::ReplayTransport>(config.transport), nullptr);
    ASSERT_TRUE(config.clock);
    EXPECT_EQ(config.clock->utc_now(), wizz::FixedClock::kDefaultUtcNow);
    EXPECT_EQ(config.data_root, options.data_root);
    EXPECT_FALSE(config.customize_client) << "request pacing stays unless --no-pacing";
}

TEST(ServiceConfig, D_30_No_pacing_removes_the_request_spacing_only)
{
    wizz::ui::AutoTestOptions options = options_with_replay();
    options.no_pacing = true;
    const ServiceConfig config = ServiceConfig::for_run(options);
    ASSERT_TRUE(config.customize_client);
    wizz::WizzClientOptions client;
    const auto retry_delays = client.retry_delays;
    config.customize_client(client);
    EXPECT_EQ(client.request_spacing, wizz::Duration::zero());
    EXPECT_EQ(client.request_jitter, wizz::Duration::zero());
    EXPECT_EQ(client.retry_delays, retry_delays) << "back-offs after 503 / 429 are kept";
}

TEST(ServiceConfig, D_67_An_autotest_run_keeps_fares_json_only_in_its_data_root_profile)
{
    wizz::ui::AutoTestOptions options = options_with_replay();
    EXPECT_TRUE(ServiceConfig::for_run(options).fare_cache_on_disk) << "an isolated profile";
    options.data_root.reset();
    EXPECT_FALSE(ServiceConfig::for_run(options).fare_cache_on_disk) << "never the user's profile";

    // The client gets the rule.
    for (const bool on_disk : {true, false}) {
        ServiceConfig config = ServiceConfig::for_run(options_with_replay());
        config.fare_cache_on_disk = on_disk;
        const auto services = wizz::app::AppServices::create(std::move(config));
        EXPECT_EQ(services->wizz->options().keep_timetables_on_disk, on_disk);
        EXPECT_EQ(services->wizz->options().cache.directory(),
                  wizz::app_paths::default_cache_dir(options_with_replay().data_root));
    }
}

} // namespace
