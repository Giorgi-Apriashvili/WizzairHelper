// PrepareAsync / ResolveAsync / PlanReturnFromAsync decisions (01 §11.4-§11.6;
// 05 §7.2 U-02; 01 §18.5 E5-E10). Fixture map CAT, en-US.

#include "wizz/ui/return_request.hpp"

#include "wizz/core/clocks.hpp"
#include "wizz/core/pricing.hpp"

#include "ui_logic/ui_samples.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {

using namespace wizz;
using namespace wizz::ui;
using test::ui::cat;
using test::ui::day;
using test::ui::en;
using test::ui::place;

[[nodiscard]] SearchInputs inputs(double from_radius = 250, double to_radius = 100)
{
    SearchInputs values;
    values.from_radius_km = from_radius;
    values.to_radius_km = to_radius;
    values.adults = 2;
    values.price_type = PriceType::wizz_discount_club;
    values.currency = "GBP";
    return values;
}

[[nodiscard]] FlightResultPtr outbound_fare_to(std::string_view to)
{
    return test::samples::builder_a("BUD", to, day(11, 20), "29.99");
}

TEST(PrepareSearch, Early_validation_texts_in_order)
{
    EXPECT_EQ(validate_search(day(11, 10), day(11, 9), "Budapest"), "The end date is before the start date.");
    EXPECT_EQ(validate_search(day(11, 10), day(11, 9), ""), "The end date is before the start date.");
    EXPECT_EQ(validate_search(day(11, 10), day(11, 10), " \t\u00A0"), "Type where you start from (a city or airport).");
    EXPECT_EQ(validate_search(day(11, 10), day(11, 20), ""), "Type where you start from (a city or airport).");
    EXPECT_FALSE(validate_search(day(11, 10), day(11, 10), "Budapest"));
}

TEST(PrepareSearch, Which_places_are_resolved)
{
    EXPECT_TRUE(should_resolve_destination("London", false, false));
    EXPECT_TRUE(should_resolve_destination("London", false, true));  // the override exists only for returns
    EXPECT_TRUE(should_resolve_destination("London", true, false));  // a typed "To" wins over a pick
    EXPECT_FALSE(should_resolve_destination("London", true, true));  // right-click flow: "To" not resolved
    EXPECT_FALSE(should_resolve_destination("", false, false));
    EXPECT_FALSE(should_resolve_destination("   ", true, false));
}

TEST(PrepareSearch, Fare_combo_index_maps_to_the_price_type)
{
    EXPECT_EQ(price_type_for_fare_index(0), PriceType::regular);
    EXPECT_EQ(price_type_for_fare_index(1), PriceType::wizz_discount_club);
}

TEST(PrepareSearch, Outbound_request_from_the_inputs)
{
    const LegSearchRequest request =
        outbound_request(place("Budapest"), place("London"), inputs(212.5, 37.5), day(11, 10), day(11, 20));
    EXPECT_EQ(request.origin, place("Budapest"));
    EXPECT_EQ(request.origin_radius_km, 212.5);  // typed fractions are kept for radii
    ASSERT_TRUE(request.destination);
    EXPECT_EQ(*request.destination, place("London"));
    EXPECT_EQ(request.destination_radius_km, 37.5);
    EXPECT_EQ(request.from, day(11, 10));
    EXPECT_EQ(request.to, day(11, 20));
    EXPECT_EQ(request.adults, 2);
    EXPECT_EQ(request.price_type, PriceType::wizz_discount_club);
    EXPECT_EQ(request.currency, "GBP");
    EXPECT_EQ(request.label, "Outbound");

    const LegSearchRequest anywhere = outbound_request(place("Budapest"), std::nullopt, inputs(), day(11, 10), day(11, 20));
    EXPECT_FALSE(anywhere.destination);
}

TEST(PrepareSearch, U_02_Return_from_a_typed_destination_keeps_the_to_radius)
{
    const auto request = return_request(place("Budapest"), place("London"), std::nullopt, std::nullopt, false,
                                        inputs(250, 100), day(11, 20), day(11, 27));
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->origin, place("London"));
    EXPECT_EQ(request->origin_radius_km, 100);
    ASSERT_TRUE(request->destination);
    EXPECT_EQ(*request->destination, place("Budapest"));  // back home
    EXPECT_EQ(request->destination_radius_km, 250);       // with the From radius
    EXPECT_EQ(request->from, day(11, 20));
    EXPECT_EQ(request->to, day(11, 27));
    EXPECT_EQ(request->adults, 2);
    EXPECT_EQ(request->price_type, PriceType::wizz_discount_club);
    EXPECT_EQ(request->currency, "GBP");
    EXPECT_EQ(request->label, "Return");

    // Same currency, adults and fare type as the outbound built from the same panel.
    const LegSearchRequest outbound =
        outbound_request(place("Budapest"), place("London"), inputs(250, 100), day(11, 10), day(11, 17));
    EXPECT_EQ(request->currency, outbound.currency);
    EXPECT_EQ(request->adults, outbound.adults);
    EXPECT_EQ(request->price_type, outbound.price_type);
    EXPECT_EQ(request->destination_radius_km, outbound.origin_radius_km);
}

TEST(PrepareSearch, U_02_Right_click_override_caps_the_start_radius_at_50_km)
{
    const Place ltn = return_origin_for(*cat(), *outbound_fare_to("LTN"));
    EXPECT_EQ(ltn.description, "London Luton (LTN), United Kingdom");
    EXPECT_EQ(ltn.pinned_airports, (std::vector<std::string>{"LTN"}));

    const auto request = return_request(place("Budapest"), place("London"), ltn, std::nullopt, true, inputs(250, 100),
                                        day(11, 22), day(11, 30));
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->origin, ltn);  // the override beats a typed "To"
    EXPECT_EQ(request->origin_radius_km, 50);
    const auto small = return_request(place("Budapest"), std::nullopt, ltn, std::nullopt, true, inputs(250, 30),
                                      day(11, 22), day(11, 30));
    EXPECT_EQ(small->origin_radius_km, 30);  // min(toRadius, 50)
}

TEST(PrepareSearch, U_02_Empty_to_uses_the_explicit_outbound_pick)
{
    const FlightResultPtr pick = outbound_fare_to("LTN");
    const std::optional<Place> picked = explicit_outbound_destination(pick, *cat());
    ASSERT_TRUE(picked);
    EXPECT_EQ(picked->description, "London Luton (LTN), United Kingdom");
    EXPECT_EQ(picked->pinned_airports, (std::vector<std::string>{"LTN"}));
    EXPECT_FALSE(explicit_outbound_destination(nullptr, *cat()));

    const auto request = return_request(place("Budapest"), std::nullopt, std::nullopt, picked, true, inputs(250, 100),
                                        day(11, 22), day(11, 30));
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->origin, *picked);
    EXPECT_EQ(request->origin_radius_km, 50);  // "To" empty: single-airport start

    // A typed "To" wins over the pick and keeps the full radius.
    const auto typed = return_request(place("Budapest"), place("London"), std::nullopt, picked, true, inputs(250, 100),
                                      day(11, 22), day(11, 30));
    EXPECT_EQ(typed->origin, place("London"));
    EXPECT_EQ(typed->origin_radius_km, 100);
}

TEST(PrepareSearch, U_02_Return_without_a_start_explains_what_to_do)
{
    const auto with_fares = return_request(place("Budapest"), std::nullopt, std::nullopt, std::nullopt, true, inputs(),
                                           day(11, 22), day(11, 30));
    ASSERT_FALSE(with_fares.has_value());
    EXPECT_EQ(with_fares.error(),
              "\u201CTo\u201D is empty (anywhere), so tell me where the return starts: click a flight in the Outbound "
              "tab (or right-click it \u2192 \u201CSearch return flights from this destination\u201D), or type the "
              "destination in \u201CTo\u201D.");
    const auto without = return_request(place("Budapest"), std::nullopt, std::nullopt, std::nullopt, false, inputs(),
                                        day(11, 22), day(11, 30));
    ASSERT_FALSE(without.has_value());
    EXPECT_EQ(without.error(), "For the return search, type your destination in \u201CTo\u201D (or run an outbound "
                               "search first and pick a flight).");
}

TEST(PrepareSearch, Right_click_origin_for_an_airport_the_map_does_not_match)
{
    FlightResult flight = *outbound_fare_to("LTN");
    Airport unknown = *pricing::unknown_airport("XYZ");
    unknown.name = "Somewhere";
    unknown.location = GeoPoint{1.5, 2.5};
    flight.to_airport = std::make_shared<const Airport>(unknown);
    const Place origin = return_origin_for(*cat(), flight);
    EXPECT_EQ(origin.name, "Somewhere");
    EXPECT_EQ(origin.description, "Somewhere (XYZ)");
    EXPECT_EQ(origin.location, (GeoPoint{1.5, 2.5}));
    EXPECT_EQ(origin.source, PlaceSource::wizz_airport);
    EXPECT_EQ(origin.pinned_airports, (std::vector<std::string>{"XYZ"}));
}

TEST(PrepareSearch, Right_click_log_line)
{
    EXPECT_EQ(plan_return_log(*outbound_fare_to("LTN"), day(11, 7), day(11, 15), en()),
              "Return will start from London Luton (LTN) (picked in the outbound results), 07 Nov \u2013 15 Nov.");
}

// ---------------------------------------------------------------------------
// ResolveAsync (01 §11.5)
// ---------------------------------------------------------------------------

TEST(ResolvePlace, No_candidates_fail_with_E5_or_E6)
{
    PlaceLookup lookup;
    lookup.lookup_failed = true;
    ResolveDecision decision = decide_resolution("  Budpest ", lookup, false, *cat());
    EXPECT_EQ(decision.action, ResolveAction::fail);
    EXPECT_EQ(decision.error, "Could not reach the place-lookup service (offline?). Type a Wizz Air city or airport "
                              "instead, e.g. \u201CLjubljana\u201D or \u201CLJU\u201D.");
    lookup.lookup_failed = false;
    decision = decide_resolution("  Grazz ", lookup, true, *cat());
    EXPECT_EQ(decision.action, ResolveAction::fail);
    EXPECT_EQ(decision.error,
              "Could not find \u201CGrazz\u201D. Check the spelling, or type a nearby city or airport.");
    EXPECT_EQ(decision.trimmed, "Grazz");
}

TEST(ResolvePlace, Accepted_correction_is_logged)
{
    PlaceLookup lookup;
    lookup.candidates = {place("London")};
    lookup.corrected_text = "London";
    ResolveDecision decision = decide_resolution(" Londn ", lookup, false, *cat());
    EXPECT_EQ(decision.action, ResolveAction::accept);
    EXPECT_EQ(decision.log, "Interpreted \u201CLondn\u201D as \u201CLondon\u201D.");

    lookup.needs_confirmation = true;  // autotest never asks
    decision = decide_resolution("Londn", lookup, true, *cat());
    EXPECT_EQ(decision.action, ResolveAction::accept);
    EXPECT_TRUE(decision.log);

    lookup.corrected_text.reset();
    lookup.needs_confirmation = false;
    decision = decide_resolution("London", lookup, false, *cat());
    EXPECT_EQ(decision.action, ResolveAction::accept);
    EXPECT_FALSE(decision.log);
}

TEST(ResolvePlace, Ambiguous_text_asks_with_the_E7_hint_for_unknown_codes)
{
    PlaceLookup lookup;
    lookup.candidates = {place("London"), place("Budapest")};
    lookup.needs_confirmation = true;
    ResolveDecision decision = decide_resolution(" stn ", lookup, false, *cat());
    EXPECT_EQ(decision.action, ResolveAction::ask);
    EXPECT_EQ(decision.trimmed, "stn");
    EXPECT_EQ(decision.hint, "\u201CSTN\u201D is not a Wizz Air airport code, so it was looked up as a place name.");

    EXPECT_FALSE(chooser_hint("BUD", *cat()));        // a catalog code
    EXPECT_FALSE(chooser_hint("lon", *cat()));        // pseudo station codes count too
    EXPECT_FALSE(chooser_hint("Wien", *cat()));       // not 3 characters
    EXPECT_FALSE(chooser_hint("AB1", *cat()));        // not all letters
    EXPECT_FALSE(chooser_hint("\U0001D400\U0001D401", *cat()));  // 4 UTF-16 units
    EXPECT_EQ(chooser_hint("\u0142\u00F3d", *cat()),
              "\u201C\u0141\u00D3D\u201D is not a Wizz Air airport code, so it was looked up as a place name.");
}

TEST(ResolvePlace, Interpreted_after_the_chooser_only_for_the_corrected_first_candidate)
{
    PlaceLookup lookup;
    lookup.candidates = {place("London"), place("Budapest")};
    lookup.corrected_text = "London";
    EXPECT_EQ(interpreted_after_choice("Londn", lookup, 0), "Interpreted \u201CLondn\u201D as \u201CLondon\u201D.");
    EXPECT_FALSE(interpreted_after_choice("Londn", lookup, 1));
    EXPECT_FALSE(interpreted_after_choice("Londn", lookup, std::nullopt));  // cancelled
    lookup.corrected_text.reset();
    EXPECT_FALSE(interpreted_after_choice("Londn", lookup, 0));
}

TEST(ResolvePlace, Progress_reports_of_the_resolve_phase)
{
    const ClientNotice notice{"Looking up \"Graz\" (Photon geocoder)", std::nullopt};
    const SearchProgress finding = finding_places_progress(notice, FixedClock::kDefaultUtcNow);
    EXPECT_EQ(finding.stage, "Finding places");
    EXPECT_EQ(finding.detail, "Looking up \"Graz\" (Photon geocoder)");
    EXPECT_EQ(finding.step_started_utc, FixedClock::kDefaultUtcNow);
    EXPECT_EQ(finding.total, 0);
    EXPECT_FALSE(finding.waiting_for_user);

    const SearchProgress waiting = waiting_for_choice_progress("Londn");
    EXPECT_EQ(waiting.stage, "Waiting for your choice");
    EXPECT_EQ(waiting.detail, "which place is \u201CLondn\u201D?");
    EXPECT_TRUE(waiting.waiting_for_user);
    EXPECT_FALSE(waiting.step_started_utc);
    EXPECT_EQ(waiting.total, 0);
}

TEST(ResolvePlace, UI_10_Fail_outcomes)
{
    EXPECT_EQ(prepare_failure("", false),
              (FailOutcome{.busy_text = "Search not started.", .success = true, .log = false, .show_error_box = false}));
    EXPECT_EQ(prepare_failure("Place lookup failed: boom", false),
              (FailOutcome{.busy_text = "Place lookup failed: boom", .success = false, .log = true, .show_error_box = true}));
    EXPECT_EQ(prepare_failure("Could not find \u201Cx\u201D. Check the spelling, or type a nearby city or airport.", true),
              (FailOutcome{.busy_text = "Could not find \u201Cx\u201D. Check the spelling, or type a nearby city or airport.",
                           .success = false,
                           .log = true,
                           .show_error_box = false}));
}

} // namespace
