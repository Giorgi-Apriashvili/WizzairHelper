// WizzApiClient::send_api, the transport state machine (04 §11): one gate turn
// and one attempt per loop pass, the ordered classification table (§11.3),
// stale-version re-discovery once per call, and the back-off schedule with its
// countdown notices (§11.4).

#include "wizz/services/wizz_client.hpp"

#include "wizz/core/format.hpp"
#include "wizz/core/text.hpp"
#include "wizz/services/wizz_protocol.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <ios>
#include <utility>

namespace wizz {

namespace {

// 04 §3.1 / §11.3 / §11.4 texts, verbatim ("—" is the EM DASH).
constexpr std::string_view kStaleVersionNotice = "Wizz Air updated its website — finding the new API version";
constexpr std::string_view kEmptyResponse = "Wizz Air sent an empty response";
constexpr std::string_view kWebPageInsteadOfData =
    "Wizz Air answered with a web page instead of data (probably a bot check). Try again later.";
constexpr std::string_view kKasadaBlocked =
    "Wizz Air's bot protection blocked the request (HTTP 429 challenge). Wait a few minutes and try again.";
constexpr std::string_view kRateLimited = "Wizz Air is rate-limiting (HTTP 429)";

constexpr std::string_view kApiOrigin = "https://www.wizzair.com";
constexpr std::string_view kApiReferer = "https://www.wizzair.com/en-gb";
constexpr std::string_view kJsonContentType = "application/json; charset=utf-8";
constexpr std::string_view kKasadaHeaderPrefix = "x-kpsdk";

/// API request headers (04 §2.3), in the order .NET sends them.
[[nodiscard]] http::Headers api_headers()
{
    return {
        {"User-Agent", std::string{kBrowserUserAgent}},
        {"Accept", "application/json, text/plain, */*"},
        {"Accept-Language", "en-GB,en;q=0.9"},
        {"Origin", std::string{kApiOrigin}},
        {"Referer", std::string{kApiReferer}},
    };
}

/// `apiUrl.TrimEnd('/') + "/" + relativePath` (04 §2.3).
[[nodiscard]] std::string api_request_url(std::string_view api_url, std::string_view relative_path)
{
    const auto end = api_url.find_last_not_of('/');
    std::string url{end == std::string_view::npos ? std::string_view{} : api_url.substr(0, end + 1)};
    url += '/';
    url += relative_path;
    return url;
}

/// The UTF-16 code units of `text` from `index`, `length` of them, back in UTF-8
/// (lone surrogates at the cut become U+FFFD, which never equals ASCII).
[[nodiscard]] std::string utf16_slice(const std::u16string& text, std::size_t index, std::size_t length)
{
    return text::utf16_to_utf8(std::u16string_view{text}.substr(index, length));
}

/// `text.StartsWith(prefix, OrdinalIgnoreCase)` over UTF-16 code units.
[[nodiscard]] bool starts_with_ignore_case(std::string_view text, std::string_view prefix)
{
    const std::u16string units = text::utf8_to_utf16(text);
    const std::size_t prefix_units = text::utf16_length(prefix);
    return units.size() >= prefix_units &&
           text::ordinal_ignore_case_equals(utf16_slice(units, 0, prefix_units), prefix);
}

/// `text.Contains(part, OrdinalIgnoreCase)` over UTF-16 code units.
[[nodiscard]] bool contains_ignore_case(std::string_view text, std::string_view part)
{
    const std::u16string units = text::utf8_to_utf16(text);
    const std::size_t part_units = text::utf16_length(part);
    if (part_units == 0) {
        return true;
    }
    for (std::size_t i = 0; i + part_units <= units.size(); ++i) {
        if (text::ordinal_ignore_case_equals(utf16_slice(units, i, part_units), part)) {
            return true;
        }
    }
    return false;
}

/// Kasada marks its challenge with response headers whose *names* start with
/// `x-kpsdk` (04 §11.3); header values are ignored (normal responses list
/// x-kpsdk-* names in Access-Control-Allow-Headers values, protocol-reference §2).
[[nodiscard]] bool is_kasada_challenge(const http::Headers& headers)
{
    return std::ranges::any_of(headers, [](const auto& header) {
        return starts_with_ignore_case(header.first, kKasadaHeaderPrefix);
    });
}

} // namespace

std::string WizzApiClient::timeout_reason() const
{
    // $"no answer within {RequestTimeout.TotalSeconds:0} s" (04 §11.4).
    const double seconds = std::chrono::duration<double>(options_.request_timeout).count();
    return std::format("no answer within {} s", format_label(seconds, 0, NumberSymbols{}));
}

// ---------------------------------------------------------------------------
// 04 §11.1 SendApi
// ---------------------------------------------------------------------------

std::string WizzApiClient::send_api(std::string_view method, std::string_view relative_path,
                                    const std::optional<std::string>& body, const NoticeSink& notices,
                                    std::stop_token stop)
{
    const std::size_t max_retries = options_.retry_delays.size();
    bool rediscovered = false;
    std::size_t retries = 0;
    for (;;) {
        throw_if_cancelled(stop);  // top of the loop: a pre-cancelled call sends nothing (CT-API-13)
        const std::string api_url = get_api_url(notices, stop);  // may run discovery (its own gate turns)
        gate_.wait_turn(stop);

        AttemptResult result = attempt(api_url, method, relative_path, body, rediscovered, stop);
        if (result.outcome == Outcome::body) {
            return std::move(result.text);
        }
        if (result.outcome == Outcome::rediscover) {
            // First non-JSON 404 of this call: the API version is gone (04 §11.3 row 3).
            rediscovered = true;
            invalidate_api_url(api_url);
            notify(notices, std::string{kStaleVersionNotice});
            continue;  // does not consume a retry
        }

        if (retries >= max_retries) {
            throw WizzApiError(
                std::format("Wizz Air request failed: {}. Gave up after {} attempts.", result.text, retries + 1));
        }
        const Duration delay = options_.retry_delays[retries];
        ++retries;
        if (result.slow_down) {
            gate_.cool_down(delay + options_.request_spacing);  // 503 / plain 429 only
        }
        // The countdown is announced before the wait starts (R-02).
        notify(notices, std::format("{} — retry {} of {}", result.text, retries, max_retries),
               options_.clock->utc_now() + delay);
        options_.delay(delay, stop);
        notify(notices, std::format("Retrying ({} of {})", retries, max_retries));
    }
}

// ---------------------------------------------------------------------------
// 04 §11.2 one attempt + 04 §11.3 classification
// ---------------------------------------------------------------------------

WizzApiClient::AttemptResult WizzApiClient::attempt(const std::string& api_url, std::string_view method,
                                                    std::string_view relative_path,
                                                    const std::optional<std::string>& body, bool rediscovered,
                                                    std::stop_token stop)
{
    const auto transient = [](std::string reason, bool slow_down = false) {
        return AttemptResult{.outcome = Outcome::transient, .text = std::move(reason), .slow_down = slow_down};
    };

    const http::Request request{
        .method = std::string{method},
        .url = api_request_url(api_url, relative_path),
        .headers = api_headers(),
        .body = body,
        .content_type = body ? std::optional<std::string>{kJsonContentType} : std::nullopt,
        .timeout = attempt_timeout(),  // DNS, connect, TLS, headers and body; not the gate wait
    };

    // Row 0: the send or the body read failed. The transport hands over a response
    // only after the whole body arrived, so a body that breaks after the headers
    // is a TransportError here and the partial response is discarded (CORE-1).
    http::Response response;
    try {
        request_count_.fetch_add(1);  // counted even if the send fails at once
        response = transport_->send(request, stop);
    } catch (const http::TimeoutError&) {
        throw_if_cancelled(stop);  // a user cancel wins even if the timeout also fired
        return transient(timeout_reason());
    } catch (const http::TransportError& error) {
        throw_if_cancelled(stop);
        return transient(std::format("network error ({})", error.what()));
    } catch (const std::ios_base::failure& error) {
        // The analogue of .NET's raw IOException; the shipped transports report
        // body failures as TransportError instead.
        throw_if_cancelled(stop);
        return transient(std::format("connection dropped ({})", error.what()));
    }
    // OperationCancelled and any other error propagate.

    const int code = response.status;
    std::string text = http::decode_body_text(response);
    const bool is_json = contains_ignore_case(http::media_type(response), "json") || protocol::looks_like_json(text);

    if (code >= 200 && code <= 299) {
        if (text::is_null_or_white_space(text)) {
            return transient(std::string{kEmptyResponse});                       // row 1
        }
        if (is_json) {
            return AttemptResult{.outcome = Outcome::body, .text = std::move(text), .slow_down = false};  // row 2
        }
        throw WizzBlockedError(std::string{kWebPageInsteadOfData}, code);         // row 2b
    }
    if (code == 404 && !is_json) {
        if (!rediscovered) {
            return AttemptResult{.outcome = Outcome::rediscover, .text = {}, .slow_down = false};  // row 3
        }
        throw WizzApiError(                                                          // row 3b
            std::format("Wizz Air API not found at {} even after looking up the current version.", api_url), 404);
    }
    if (code == 400 && is_json) {
        std::vector<std::string> codes = protocol::parse_validation_codes(text);
        if (!codes.empty()) {
            throw WizzValidationError(std::move(codes));                             // row 4
        }
        throw WizzApiError(std::format("Wizz Air rejected the request (HTTP 400): {}", protocol::snippet(text)),
                           400);                                                     // row 4b
    }
    if (code == 429) {
        if (is_kasada_challenge(response.headers)) {
            throw WizzBlockedError(std::string{kKasadaBlocked}, 429);                // row 5: never retried
        }
        return transient(std::string{kRateLimited}, true);                          // row 5b
    }
    if (code >= 500 || code == 408) {
        return transient(std::format("Wizz Air is busy (HTTP {})", code), code == 503);  // row 6
    }
    if (!is_json) {
        throw WizzBlockedError(                                                      // row 7
            std::format("Wizz Air refused the request (HTTP {}). Your network may be blocked; try again later.", code),
            code);
    }
    throw WizzApiError(std::format("Unexpected Wizz Air response (HTTP {}): {}", code, protocol::snippet(text)),
                       code);                                                        // row 8
}

} // namespace wizz
