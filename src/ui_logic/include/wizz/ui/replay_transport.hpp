#pragma once
// ReplayTransport: an http::Transport that answers from a fixture directory
// instead of the network (DECISIONS D-30, `--replay <dir>`), so the real GUI can
// run its --autotest script offline and isolated (05 §5: "CI uses fixtures only",
// "cached replay").
//
// Directory format — `<dir>/replay.json` plus the body files it names:
//
//   {
//     "rules": [
//       { "urlPrefix": "https://www.wizzair.com", "file": "home.html" },
//       { "urlContains": "/asset/map", "file": "map_subset.json" },
//       { "method": "POST", "urlContains": "/search/timetableV2",
//         "bodyContains": "\"departureStation\":\"BUD\"", "file": "timetable_v2_bud_ltn.json" },
//       { "urlContains": "/search/timetableV2", "status": 503, "body": "", "times": 1 },
//       { "urlContains": "/search/timetableV2", "status": 400,
//         "body": "{\"validationCodes\":[\"InvalidMarket\"]}" },
//       { "urlContains": "frankfurter", "file": "fx_frankfurter_v2.json", "delayMs": 1500 },
//       { "urlContains": "photon", "error": "transport", "message": "offline" }
//     ]
//   }
//
// Rules are tried in file order; the first rule whose matchers ALL hold (and
// that is not used up) answers. Matchers (ordinal, case-sensitive; omitted =
// any): "method", "url" (exact), "urlPrefix", "urlContains", "bodyContains"
// (a request without a body never matches it). Answer fields:
//   "status"      HTTP status, default 200;
//   "file"        body file, relative to <dir>, read byte for byte at load time;
//   "body"        inline UTF-8 body (instead of "file"; default "");
//   "contentType" Content-Type; default by the file extension (.json →
//                 "application/json; charset=utf-8", .html/.htm → "text/html;
//                 charset=utf-8", other files → "text/plain; charset=utf-8"),
//                 "application/json; charset=utf-8" for an inline body; JSON null
//                 sends no Content-Type at all;
//   "headers"     object of extra RESPONSE headers, e.g. {"x-kpsdk-ct": "abc"};
//   "delayMs"     wait this long first (cancellable: the stop token throws
//                 wizz::OperationCancelled), to replay a slow server;
//   "error"       "transport" or "timeout": throw http::TransportError /
//                 http::TimeoutError with "message" (default "replayed failure")
//                 instead of answering;
//   "times"       answer at most this many matching requests, then skip the rule;
//   "comment"     free text, ignored.
// A request no rule answers gets 404 `{"message":"No HTTP resource was found"}`
// (application/json; charset=utf-8), like the test suite's default responder
// (05 §1.6 rule 4). Unknown fields are rejected so typos surface at start-up.
//
// Thread-safe: send() may run on several workers at once.

#include "wizz/core/http.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wizz::ui {

/// A replay directory that cannot be used (missing or malformed replay.json, unknown
/// field, unreadable body file). The message names the problem.
class ReplayConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ReplayTransport final : public http::Transport {
public:
    /// The manifest file name inside the replay directory.
    static constexpr std::string_view kManifestName = "replay.json";

    struct Rule {
        std::optional<std::string> method;
        std::optional<std::string> url;
        std::optional<std::string> url_prefix;
        std::optional<std::string> url_contains;
        std::optional<std::string> body_contains;
        int status = 200;
        std::string body;
        std::optional<std::string> content_type;
        http::Headers headers;
        std::chrono::milliseconds delay{0};
        enum class Failure { none, transport, timeout } failure = Failure::none;
        std::string failure_message{"replayed failure"};
        std::optional<std::size_t> times;
    };

    /// Loads `<directory>/replay.json` and every body file. Throws ReplayConfigError.
    explicit ReplayTransport(const std::filesystem::path& directory);
    /// Rules built in code (tests).
    explicit ReplayTransport(std::vector<Rule> rules);

    /// Answers `request` from the first matching rule (see the file comment).
    [[nodiscard]] http::Response send(const http::Request& request, std::stop_token stop) override;

    /// Requests seen so far, in arrival order (URLs only; diagnostics).
    [[nodiscard]] std::vector<std::string> urls() const;
    [[nodiscard]] std::size_t request_count() const;
    [[nodiscard]] const std::vector<Rule>& rules() const noexcept { return rules_; }

private:
    [[nodiscard]] static bool matches(const Rule& rule, const http::Request& request);

    std::vector<Rule> rules_;
    mutable std::mutex mutex_;
    std::vector<std::size_t> used_;  ///< answers given per rule
    std::vector<std::string> urls_;
};

} // namespace wizz::ui
