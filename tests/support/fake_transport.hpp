#pragma once
// Fake HTTP transport for the offline conformance suite (05 §1.2, §1.12).
// Every request is recorded *before* the responder runs, so a test can assert
// that nothing was sent even when its responder would throw (CT-GEOC-01).

#include "wizz/core/http.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wizz::test {

/// One request as the fake saw it (05 §1.2.1).
struct RecordedRequest {
    std::string method;
    std::string url;                          ///< absolute URL as sent
    std::optional<std::string> body;          ///< UTF-8 body, nullopt when there is none
    http::Headers headers;                    ///< request headers in send order
    std::optional<std::string> content_type;  ///< content header (Content-Type)

    /// Values of every request header named `name` (ASCII case-insensitive), in send order.
    [[nodiscard]] std::vector<std::string> header_values(std::string_view name) const;
};

/// What a responder hands back (05 §1.12 `ScriptedResponse`): a complete
/// response, or one whose body breaks after the status and headers arrived.
struct ScriptedResponse {
    http::Response response;
    /// When set, reading the body fails part-way (05 §1.2.4): FakeTransport::send
    /// throws http::TransportError with this message instead of returning.
    std::optional<std::string> body_error;

    /// Copy with one more *response* header (not a content header), e.g. `x-kpsdk-ct: abc` (05 §1.2.5).
    [[nodiscard]] ScriptedResponse with_header(std::string name, std::string value) const;
};

/// `Json(body, status)`: Content-Type `application/json; charset=utf-8`.
[[nodiscard]] ScriptedResponse json_response(std::string body, int status = 200);
/// `Html(body, status)`: Content-Type `text/html; charset=utf-8`.
[[nodiscard]] ScriptedResponse html_response(std::string body, int status = 200);
/// .NET `new HttpResponseMessage(status) { Content = new StringContent("") }`: empty
/// body with Content-Type `text/plain; charset=utf-8` (CT-API-07, CT-FSS-02).
[[nodiscard]] ScriptedResponse string_content_response(int status);
/// A bare `new HttpResponseMessage(status)`: empty body and **no** Content-Type (CT-FX-05).
[[nodiscard]] ScriptedResponse bare_response(int status);
/// `BrokenStream` (05 §1.2.4, CT-SRCH-03): 200 `application/json` whose body breaks
/// after `kBrokenStreamPrefix`; the client sees http::TransportError
/// "Error while copying content to a stream." from send().
[[nodiscard]] ScriptedResponse broken_stream_response();

/// The 28 bytes a BrokenStream delivers before failing.
inline constexpr std::string_view kBrokenStreamPrefix = R"({"outboundFlights":[{"depart)";
/// The error .NET raises when the body read fails part-way.
inline constexpr std::string_view kBrokenStreamError = "Error while copying content to a stream.";

/// Test-supplied answer function. It may throw http::TransportError (the
/// "offline" network failure the client must treat as transient) or any other
/// exception (e.g. std::logic_error("must not be called")). If a test sends from
/// several threads, the responder must be thread-safe itself.
using Responder = std::function<ScriptedResponse(const RecordedRequest&)>;

class FakeTransport final : public http::Transport {
public:
    explicit FakeTransport(Responder responder);

    /// Records the request (thread-safe), then throws OperationCancelled if `stop`
    /// was requested, otherwise returns or throws whatever the responder scripted.
    [[nodiscard]] http::Response send(const http::Request& request, std::stop_token stop) override;

    /// Snapshot of every recorded request, in arrival order.
    [[nodiscard]] std::vector<RecordedRequest> requests() const;
    [[nodiscard]] std::size_t request_count() const;
    /// Snapshot of the recorded URLs, in arrival order.
    [[nodiscard]] std::vector<std::string> urls() const;
    /// Number of recorded requests whose URL contains `fragment` (ordinal), e.g. "/search/timetableV2".
    [[nodiscard]] std::size_t count_urls_containing(std::string_view fragment) const;

private:
    Responder responder_;
    mutable std::mutex mutex_;
    std::vector<RecordedRequest> requests_;
};

} // namespace wizz::test
