// "Homes on Airbnb" end to end in the window (DECISIONS D-72): each grid opens
// Airbnb's search around the destination city's centre, found through the
// geocoder (replayed) and remembered; without a centre Airbnb searches by name;
// the lookup runs beside a search, survives Cancel and dies with the window.

#include "app_test_support.hpp"
#include "grid_bar_button.hpp"

#include "support/fixtures.hpp"

#include "wizz/core/airbnb_links.hpp"
#include "wizz/core/city_names.hpp"
#include "wizz/core/http.hpp"
#include "wizz/ui/homes_stay.hpp"
#include "wizz/ui/strings.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using wizz::Date;
using wizz::FlightResultPtr;
using wizz::GeoPoint;
using wizz::test::app::json_rule;
using wizz::test::app::scenario_rules;
using wizz::test::app::WindowRig;
using wizz::test::app::WindowRigOptions;
using wizz::ui::HomesRadius;
using wizz::ui::HomesStay;
using wizz::ui::StayLeg;
namespace links = wizz::airbnb_links;
namespace strings = wizz::ui::strings;
namespace gui = wizz::test::gui;
using Rules = std::vector<wizz::ui::ReplayTransport::Rule>;

/// The first hit of geo_photon_london.json: central London.
const GeoPoint kLondon{51.5074456, -0.1277653};

/// The 03 §11 searches (Budapest r200 → London r30): outbound and return.
void run_searches(WindowRig& rig)
{
    rig.input().set_from_radius_km(200);
    rig.input().set_to_radius_km(30);
    rig.input().apply_autotest_dates(
        wizz::ui::DateWindow{.start = Date::from_ymd(2026, 11, 10), .end = Date::from_ymd(2026, 11, 20)},
        wizz::ui::DateWindow{.start = Date::from_ymd(2026, 11, 12), .end = Date::from_ymd(2026, 11, 20)});
    rig.run(rig.controller().search_outbound());
    rig.run(rig.controller().search_return());
}

[[nodiscard]] Rules with_geocoder(Rules extra)
{
    Rules rules = scenario_rules("named_destination");
    rules.insert(rules.begin(), extra.begin(), extra.end());
    return rules;
}

[[nodiscard]] std::string stay_text(const HomesStay& stay)
{
    const wizz::Locale& locale = *wizz::Locale::en_us();
    return strings::homes_stay_text(locale.format_date(stay.check_in, "dd MMM"),
                                    locale.format_date(stay.check_out, "dd MMM"), stay.adults);
}

[[nodiscard]] std::string box_url(const HomesStay& stay, double radius_km)
{
    return links::homes_in_box(links::box_around(kLondon, radius_km), stay.check_in, stay.check_out, stay.adults);
}

[[nodiscard]] std::size_t count_lines(const WindowRig& rig, const std::string& line)
{
    const std::vector<std::string> lines = rig.log_lines();
    return static_cast<std::size_t>(std::ranges::count(lines, line));
}

[[nodiscard]] std::string lookup_line(const FlightResultPtr& fare, bool inbound = false)
{
    return strings::homes_lookup_log(wizz::city_names::clean_name(inbound ? fare->from_airport->name : fare->to_airport->name));
}

/// Holds timetable and/or geocoder requests until released; a cancelled request gives up.
class HoldingTransport final : public wizz::http::Transport {
public:
    explicit HoldingTransport(std::shared_ptr<wizz::http::Transport> inner) : inner_(std::move(inner)) {}

    wizz::http::Response send(const wizz::http::Request& request, std::stop_token stop) override
    {
        const bool timetable = request.url.contains("timetable");
        const bool geocoder = request.url.contains("photon") || request.url.contains("nominatim");
        if (timetable || geocoder) {
            std::unique_lock lock{mutex_};
            ++(timetable ? held_timetables_ : held_geocodes_);
            const bool released = changed_.wait(lock, stop, [&] { return timetable ? !hold_timetables_ : !hold_geocodes_; });
            if (!released) {
                wizz::throw_if_cancelled(stop);
            }
        }
        return inner_->send(request, stop);
    }

    void hold(bool timetables, bool geocodes)
    {
        {
            const std::lock_guard lock{mutex_};
            hold_timetables_ = timetables;
            hold_geocodes_ = geocodes;
        }
        changed_.notify_all();
    }
    [[nodiscard]] int held_timetables() const
    {
        const std::lock_guard lock{mutex_};
        return held_timetables_;
    }
    [[nodiscard]] int held_geocodes() const
    {
        const std::lock_guard lock{mutex_};
        return held_geocodes_;
    }

private:
    std::shared_ptr<wizz::http::Transport> inner_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    bool hold_timetables_ = false;
    bool hold_geocodes_ = false;
    int held_timetables_ = 0;
    int held_geocodes_ = 0;
};

TEST(HomesOnAirbnb, D_72_Each_grid_opens_Airbnb_around_the_city_centre_and_remembers_it)
{
    WindowRigOptions options;
    options.rules = with_geocoder({json_rule("photon.komoot.io", wizz::test::read_fixture("geo_photon_london.json"))});
    WindowRig rig{std::move(options)};
    rig.start();
    run_searches(rig);
    const Date today = rig.services().clock->local_today();
    const int min_nights = rig.input().min_nights();

    // Outbound: the arrival city, the minimum nights from the flight's date.
    const FlightResultPtr outbound = rig.outbound().selected();
    ASSERT_NE(outbound, nullptr);
    const std::optional<HomesStay> out_stay = wizz::ui::stay_for_flight(*outbound, StayLeg::outbound, min_nights, today);
    ASSERT_TRUE(out_stay.has_value());
    ASSERT_TRUE(rig.outbound().homes_button()->isEnabled());
    rig.outbound().homes_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 1; }));
    EXPECT_EQ(rig.actions.opened[0].toStdString(), box_url(*out_stay, 5.0));
    EXPECT_EQ(count_lines(rig, lookup_line(outbound)), 1u);
    EXPECT_TRUE(rig.log_contains(strings::homes_centre_log("5 km", "London", stay_text(*out_stay))));
    EXPECT_FALSE(rig.controller().is_finding_homes());

    // Return: the departure city, the minimum nights before the flight's date.
    const FlightResultPtr inbound = rig.inbound().selected();
    ASSERT_NE(inbound, nullptr);
    const std::optional<HomesStay> back_stay = wizz::ui::stay_for_flight(*inbound, StayLeg::inbound, min_nights, today);
    ASSERT_TRUE(back_stay.has_value());
    rig.inbound().prepare_context_menu();
    rig.inbound().request_homes();
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 2; }));
    EXPECT_EQ(rig.actions.opened[1].toStdString(), box_url(*back_stay, 5.0));

    // Round trips: from the outbound to the return.
    const wizz::RoundTrip* trip = rig.trips().selected();
    ASSERT_NE(trip, nullptr);
    rig.trips().homes_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 3; }));
    EXPECT_EQ(rig.actions.opened[2].toStdString(), box_url(*wizz::ui::stay_for_trip(*trip, today), 5.0));

    // Again for the same station: no new lookup.
    rig.outbound().homes_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 4; }));
    EXPECT_EQ(rig.actions.opened[3], rig.actions.opened[0]);
    EXPECT_EQ(count_lines(rig, lookup_line(outbound)), 1u) << "remembered for this session";
    EXPECT_TRUE(rig.flow_errors.empty());
}

TEST(HomesOnAirbnb, D_73_The_distance_comes_from_the_settings)
{
    WindowRigOptions options;
    options.rules = with_geocoder({json_rule("photon.komoot.io", wizz::test::read_fixture("geo_photon_london.json"))});
    options.settings.homes_within = HomesRadius::twenty_five_km;
    WindowRig rig{std::move(options)};
    rig.start();
    run_searches(rig);
    const std::optional<HomesStay> stay = rig.outbound().selected_stay();
    ASSERT_TRUE(stay.has_value());
    rig.outbound().request_homes();
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 1; }));
    EXPECT_EQ(rig.actions.opened[0].toStdString(), box_url(*stay, 25.0));
    EXPECT_TRUE(rig.log_contains(strings::homes_centre_log("25 km", "London", stay_text(*stay))));
}

TEST(HomesOnAirbnb, D_72_Without_a_city_centre_Airbnb_searches_by_name)
{
    WindowRigOptions options;
    options.rules = with_geocoder({json_rule("photon.komoot.io", R"({"type":"FeatureCollection","features":[]})"),
                                   json_rule("nominatim.openstreetmap.org", "[]")});
    WindowRig rig{std::move(options)};
    rig.start();
    run_searches(rig);
    const FlightResultPtr fare = rig.outbound().selected();
    const std::optional<HomesStay> stay = rig.outbound().selected_stay();
    ASSERT_TRUE(stay.has_value());
    rig.outbound().request_homes();
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 1; }));
    const std::string query =
        links::place_query(wizz::city_names::clean_name(fare->to_airport->name), fare->to_airport->country_name);
    EXPECT_EQ(rig.actions.opened[0].toStdString(),
              links::homes_by_name(query, stay->check_in, stay->check_out, stay->adults));
    EXPECT_TRUE(rig.log_contains(strings::homes_by_name_log(query, false, stay_text(*stay))));

    // "No city" is remembered: the second request looks nothing up.
    rig.outbound().request_homes();
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 2; }));
    EXPECT_EQ(count_lines(rig, lookup_line(fare)), 1u);
}

TEST(HomesOnAirbnb, D_72_Unreachable_geocoders_search_by_name_and_are_tried_again_next_time)
{
    WindowRig rig;  // no geocoder rules: every lookup gets HTTP 404, a service failure
    rig.start();
    run_searches(rig);
    const FlightResultPtr fare = rig.outbound().selected();
    const std::optional<HomesStay> stay = rig.outbound().selected_stay();
    ASSERT_TRUE(stay.has_value());
    rig.outbound().request_homes();
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 1; }));
    const std::string query =
        links::place_query(wizz::city_names::clean_name(fare->to_airport->name), fare->to_airport->country_name);
    EXPECT_EQ(rig.actions.opened[0].toStdString(),
              links::homes_by_name(query, stay->check_in, stay->check_out, stay->adults));
    EXPECT_TRUE(rig.log_contains(strings::homes_by_name_log(query, true, stay_text(*stay))));

    rig.outbound().request_homes();
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 2; }));
    EXPECT_EQ(count_lines(rig, lookup_line(fare)), 2u) << "a failure is not remembered";
}

/// Every geocoder request fails with an error that is not a service failure: it escapes the
/// place resolver and the city-centre lookup.
class BrokenGeocoder final : public wizz::http::Transport {
public:
    explicit BrokenGeocoder(std::shared_ptr<wizz::http::Transport> inner) : inner_(std::move(inner)) {}

    wizz::http::Response send(const wizz::http::Request& request, std::stop_token stop) override
    {
        if (request.url.contains("photon") || request.url.contains("nominatim")) {
            throw std::logic_error("broken geocoder");
        }
        return inner_->send(request, stop);
    }

private:
    std::shared_ptr<wizz::http::Transport> inner_;
};

TEST(HomesOnAirbnb, D_72_An_unexpected_lookup_error_searches_by_name_and_is_tried_again_next_time)
{
    WindowRigOptions options;
    options.wrap_transport = [](std::shared_ptr<wizz::http::Transport> inner) {
        return std::make_shared<BrokenGeocoder>(std::move(inner));
    };
    WindowRig rig{std::move(options)};
    rig.start();
    run_searches(rig);
    const FlightResultPtr fare = rig.outbound().selected();
    const std::optional<HomesStay> stay = rig.outbound().selected_stay();
    ASSERT_TRUE(stay.has_value());
    rig.outbound().request_homes();
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 1; }));
    const std::string query =
        links::place_query(wizz::city_names::clean_name(fare->to_airport->name), fare->to_airport->country_name);
    EXPECT_EQ(rig.actions.opened[0].toStdString(),
              links::homes_by_name(query, stay->check_in, stay->check_out, stay->adults));
    EXPECT_TRUE(rig.log_contains(strings::homes_by_name_log(query, true, stay_text(*stay))));
    EXPECT_FALSE(rig.controller().is_finding_homes());
    EXPECT_TRUE(rig.flow_errors.empty()) << "the flow handles it";

    rig.outbound().request_homes();
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 2; }));
    EXPECT_EQ(count_lines(rig, lookup_line(fare)), 2u) << "not remembered";
}

TEST(HomesOnAirbnb, D_72_The_lookup_runs_beside_a_search_and_Cancel_stops_only_the_search)
{
    std::shared_ptr<HoldingTransport> holding;
    WindowRigOptions options;
    options.rules = with_geocoder({json_rule("photon.komoot.io", wizz::test::read_fixture("geo_photon_london.json"))});
    options.wrap_transport = [&holding](std::shared_ptr<wizz::http::Transport> inner) {
        holding = std::make_shared<HoldingTransport>(std::move(inner));
        return holding;
    };
    WindowRig rig{std::move(options)};
    rig.start();
    run_searches(rig);
    const std::optional<HomesStay> stay = rig.outbound().selected_stay();
    ASSERT_TRUE(stay.has_value());

    // A new search waits on its first fare request; the homes lookup waits on the geocoder.
    holding->hold(true, true);
    wizz::app::Task<> search = rig.controller().search_outbound();
    search.start();
    ASSERT_TRUE(gui::wait_until([&] { return holding->held_timetables() > 0; }));
    ASSERT_TRUE(rig.controller().is_working());
    rig.controller().request_homes(*stay, HomesRadius::five_km, rig.actions.actions());
    ASSERT_TRUE(gui::wait_until([&] { return holding->held_geocodes() > 0; }));
    EXPECT_TRUE(rig.controller().is_finding_homes());

    // A second request meanwhile is dropped.
    rig.controller().request_homes(*stay, HomesRadius::two_km, rig.actions.actions());

    // Cancel (Esc) stops the search, not the lookup.
    rig.controller().cancel_work();
    ASSERT_TRUE(gui::wait_until([&] { return search.done(); }));
    EXPECT_FALSE(rig.controller().is_working());
    EXPECT_TRUE(rig.controller().is_finding_homes());
    EXPECT_TRUE(rig.actions.opened.empty());

    holding->hold(false, false);
    ASSERT_TRUE(gui::wait_until([&] { return rig.actions.opened.size() == 1; }));
    EXPECT_EQ(rig.actions.opened[0].toStdString(), box_url(*stay, 5.0)) << "the first request's 5 km";
    gui::settle();
    EXPECT_EQ(rig.actions.opened.size(), 1u) << "the dropped request opened nothing";
    EXPECT_FALSE(rig.controller().is_finding_homes());
    EXPECT_TRUE(rig.flow_errors.empty());
}

TEST(HomesOnAirbnb, D_72_Closing_the_window_stops_the_lookup_and_opens_nothing)
{
    std::shared_ptr<HoldingTransport> holding;
    WindowRigOptions options;
    options.rules = with_geocoder({json_rule("photon.komoot.io", wizz::test::read_fixture("geo_photon_london.json"))});
    options.wrap_transport = [&holding](std::shared_ptr<wizz::http::Transport> inner) {
        holding = std::make_shared<HoldingTransport>(std::move(inner));
        return holding;
    };
    WindowRig rig{std::move(options)};
    rig.start();
    run_searches(rig);
    holding->hold(false, true);
    rig.outbound().request_homes();
    ASSERT_TRUE(gui::wait_until([&] { return holding->held_geocodes() > 0; }));

    rig.window().close();
    ASSERT_TRUE(gui::wait_until([&] { return !rig.controller().is_finding_homes(); }));
    holding->hold(false, false);
    gui::settle();
    EXPECT_TRUE(rig.actions.opened.empty());
    EXPECT_TRUE(rig.flow_errors.empty());

    // After the window closed, a request does nothing.
    rig.outbound().request_homes();
    gui::settle();
    EXPECT_TRUE(rig.actions.opened.empty());
}

} // namespace
