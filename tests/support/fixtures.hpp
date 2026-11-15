#pragma once
// Fixture access and the standard responders of the conformance suite
// (05 §1.6, §1.7, §2). Fixtures are copied next to the test binary post-build
// and read byte for byte; tests never modify them.

#include "support/fake_transport.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace wizz::test {

/// `<directory of the running test executable>/Fixtures`.
[[nodiscard]] std::filesystem::path fixtures_dir();

/// `Fixtures.Read(name)`: the whole file `<test-exe-dir>/Fixtures/<name>`, byte-exact
/// (no newline translation, BOM kept). Throws std::runtime_error naming the path if it
/// cannot be read.
[[nodiscard]] std::string read_fixture(std::string_view name);

/// `HOME_HTML` (05 §1.6): the synthetic wizzair.com homepage advertising API 29.23.1.
inline constexpr std::string_view kHomeHtml =
    R"(<html><script>window.__NUXT__={};window.__NUXT__.config={public:{baseUrl:"https://www.wizzair.com",apiUrl:"https://be.wizzair.com/29.23.1/Api",x:1}}</script></html>)";

/// `HomeHtmlFor(v)`: HOME_HTML with `29.23.1` replaced by `version`.
[[nodiscard]] std::string home_html_for(std::string_view version);

/// The timetable fixture the default responder serves unless a test picks another.
inline constexpr std::string_view kDefaultTimetableFixture = "timetable_v2_bud_ltn.json";

/// `TestClients.Default` (05 §1.6), rules in this order:
/// 1. URL starts with `https://www.wizzair.com` → 200 html HOME_HTML;
/// 2. URL contains `/asset/map` → 200 json `map_subset.json`;
/// 3. URL contains `/search/timetableV2` → 200 json `timetable_fixture`;
/// 4. anything else → 404 json `{"message":"No HTTP resource was found"}`.
/// The fixtures are read once, when the responder is created.
[[nodiscard]] Responder default_responder(std::string_view timetable_fixture = kDefaultTimetableFixture);

} // namespace wizz::test
