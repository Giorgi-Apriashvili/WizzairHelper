#pragma once
// The app's single HTTP client (04 §0.1, §1 "Wiring", §2; 06 §3): a libcurl
// implementation of wizz::http::Transport shared by the Wizz client, FX and
// geocoding.

#include "wizz/core/http.hpp"
#include "wizz/services/windows_proxy.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace wizz::http {

/// libcurl transport with the .NET HttpClient behaviour the spec relies on (04 §2):
/// - HTTP/1.1 only (D-21); cookies are never stored or sent.
/// - Sends `Accept-Encoding: gzip, deflate, br` after the request headers and returns
///   the decoded body; POST bodies go out as-is with Content-Length (no
///   `Expect: 100-continue`, no chunking) and `request.content_type`. libcurl adds
///   only Host (and Content-Length); no default User-Agent or Accept is sent.
/// - `request.timeout` bounds the whole send — proxy discovery, DNS, connect, TLS,
///   every redirect, headers and body (04 §11.2) — and raises TimeoutError, as does
///   `request.connect_timeout` for one connection attempt. Any other failure,
///   including a body that breaks after the headers arrived, raises TransportError
///   with libcurl's description.
/// - `stop` aborts within ~100 ms with wizz::OperationCancelled, also while the proxy
///   is being looked up; a user cancel wins over a timeout or a result that arrives at
///   the same time (04 §11.2).
/// - Redirects are followed like .NET (04 §2.2): at most 50; only to http/https and
///   never from https to http; 300/301/302 after POST and 303 after anything but GET
///   become a GET without body or Content-Type; 307/308 repeat the request. A
///   redirect that is not followed is returned as the response.
/// - The response carries the final status, the final response's headers in wire
///   order (earlier redirect, 1xx and proxy CONNECT responses are dropped),
///   Content-Type separately, and the decoded body bytes.
/// - TLS via Schannel and the Windows trust store, peer and host verified;
///   revocation is checked where the distribution point is reachable.
/// - Proxy: windows_proxy.hpp rules; loopback hosts always connect directly.
///   Proxies requiring Negotiate/NTLM authentication get the Windows user's
///   credentials. WinHTTP proxy detection / setup-script evaluation (and an injected
///   ProxySelector) runs on a helper thread before each transfer; send() stops waiting
///   for it at the timeout or on `stop`, and the abandoned lookup finishes in the
///   background (WinHTTP caches its results).
///
/// Thread-safe: concurrent send() calls run separate transfers. Idle keep-alive
/// connections (5 min lifetime) and DNS results are reused across calls. The
/// destructor waits for proxy lookups that are still running.
class CurlTransport final : public Transport {
public:
    /// The proxy for one request URL: nullopt leaves the choice to libcurl (environment
    /// variables), "" connects directly, anything else is the proxy URL. It may block.
    using ProxySelector = std::function<std::optional<std::string>(const std::string& url)>;

    /// Proxy from the environment or the current user's Windows settings, read once.
    CurlTransport();
    /// Proxy exactly as in `proxy`; environment variables and Windows settings are ignored.
    explicit CurlTransport(WindowsProxyConfig proxy);
    /// Proxy chosen by `select_proxy` for every request, loopback hosts included
    /// (tests use it to simulate slow proxy discovery).
    explicit CurlTransport(ProxySelector select_proxy);
    ~CurlTransport() override;

    CurlTransport(const CurlTransport&) = delete;
    CurlTransport& operator=(const CurlTransport&) = delete;

    /// Sends a GET or POST (std::invalid_argument for other methods or header
    /// fields containing line breaks).
    [[nodiscard]] Response send(const Request& request, std::stop_token stop) override;

    /// libcurl's version line, e.g. "libcurl/8.23.0 Schannel zlib/1.3.2 brotli/1.2.0",
    /// for diagnostics.
    [[nodiscard]] static std::string library_version();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wizz::http
