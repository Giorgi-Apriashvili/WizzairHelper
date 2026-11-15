#pragma once
// Wizz Air client errors (04 §10, 05 §1.9). The hierarchy mirrors the .NET
// exception types the search loop and the UI catch, in this catch order:
//
//   WizzApiError {optional status}      generic client failure (WizzApiException)
//    ├── WizzBlockedError               bot check / WAF / non-data page; never retried
//    └── WizzValidationError {codes}    HTTP 400 {"validationCodes":[...]}; status 400
//
// User cancellation is wizz::OperationCancelled (cancellation.hpp) and never
// derives from WizzApiError. Transport failures (http::TransportError,
// http::TimeoutError) never escape the client: it turns them into retries.
// Tests assert the *exact* dynamic type (typeid), so the two subclasses are final
// and nothing else derives from WizzApiError.

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace wizz {

/// `WizzApiException(message, statusCode?)`: any failure of a Wizz Air call that
/// is not a cancellation — retry exhaustion, an unexpected status, a discovery
/// failure, an unreadable timetable or route map (04 §10, §11.3).
class WizzApiError : public std::runtime_error {
public:
    explicit WizzApiError(const std::string& message, std::optional<int> status = std::nullopt)
        : std::runtime_error(message), status_(status)
    {
    }

    /// The HTTP status the error stands for; nullopt when it has none (discovery
    /// failure, retry exhaustion even after a 502, unreadable bodies).
    [[nodiscard]] std::optional<int> status() const noexcept { return status_; }

private:
    std::optional<int> status_;
};

/// `WizzBlockedException`: Kasada 429, a 2xx web page instead of data, or a
/// non-JSON 4xx (04 §11.3 rows 2b, 5, 7). Retrying will not help; the search
/// aborts and the route-map loader falls back to a stale copy.
class WizzBlockedError final : public WizzApiError {
public:
    WizzBlockedError(const std::string& message, std::optional<int> status) : WizzApiError(message, status) {}
};

/// `WizzValidationException`: HTTP 400 with a non-empty `validationCodes` array
/// (04 §11.3 row 4). Message `Wizz Air rejected the request: ` + codes joined
/// with ", " (response order, non-string entries already dropped); status 400.
class WizzValidationError final : public WizzApiError {
public:
    explicit WizzValidationError(std::vector<std::string> codes);

    /// The codes in response order.
    [[nodiscard]] const std::vector<std::string>& codes() const noexcept { return *codes_; }

    /// `Has(code)`: true when some code equals `code` under OrdinalIgnoreCase (04 §9.2).
    [[nodiscard]] bool has(std::string_view code) const noexcept;

private:
    // Shared so that copying the exception object never throws.
    std::shared_ptr<const std::vector<std::string>> codes_;
};

} // namespace wizz
