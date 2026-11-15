#pragma once
// Abstract HTTP transport (06 §1). The production implementation is libcurl
// (services: CurlTransport); tests inject a fake that records every request
// before answering (05 §1.2). Body decoding rules follow 04 §2.2.

#include <chrono>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wizz::http {

/// Header list in wire order. Names keep the casing they were sent / received with.
using Headers = std::vector<std::pair<std::string, std::string>>;

struct Request {
    std::string method;                       ///< "GET" or "POST"
    std::string url;                          ///< absolute URL
    Headers headers;                          ///< request headers in send order (without Content-Type)
    std::optional<std::string> body;          ///< UTF-8 body; nullopt = no body
    std::optional<std::string> content_type;  ///< e.g. "application/json; charset=utf-8"
    std::chrono::milliseconds timeout{30'000};          ///< whole attempt: DNS, connect, TLS, headers and body
    std::chrono::milliseconds connect_timeout{15'000};  ///< per connection attempt
};

struct Response {
    int status = 0;
    Headers headers;                          ///< response headers (Content-Type excluded)
    std::optional<std::string> content_type;  ///< Content-Type header value, nullopt if absent
    std::string body;                         ///< raw bytes after transfer decoding (gzip/deflate/br)
};

/// Network, DNS, TLS or I/O failure — including a body that breaks after the
/// headers arrived (05 §1.2.4). The client treats it as transient.
class TransportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The connect timeout or the per-attempt timeout elapsed (not a user cancel).
class TimeoutError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Sends one request. Throws TransportError, TimeoutError, or
/// wizz::OperationCancelled when `stop` fires (user cancel wins over a timeout).
class Transport {
public:
    virtual ~Transport() = default;
    [[nodiscard]] virtual Response send(const Request& request, std::stop_token stop) = 0;
};

/// Media type without parameters, trimmed; "" when Content-Type is missing (04 §2.2).
[[nodiscard]] std::string media_type(const Response& response);

/// Decodes the body to UTF-8 text like .NET ReadAsStringAsync: honour the
/// charset parameter (utf-8, iso-8859-1/latin1; unknown → UTF-8), otherwise
/// detect and strip a UTF-8/16/32 BOM, default UTF-8, invalid sequences → U+FFFD.
[[nodiscard]] std::string decode_body_text(const Response& response);

/// RFC 3986 escaping like Uri.EscapeDataString: every UTF-8 byte except
/// A–Z a–z 0–9 - . _ ~ becomes %XX with upper-case hex.
[[nodiscard]] std::string escape_data_string(std::string_view text);

} // namespace wizz::http
