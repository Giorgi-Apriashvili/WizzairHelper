#pragma once
// A scripted HTTP/1.1 server on 127.0.0.1 (ephemeral port) for transport tests; it
// never touches anything but the loopback interface. Each accepted connection is
// served on its own thread: every request on it (headers plus a Content-Length body)
// is recorded and handed to the test's handler, which answers with raw bytes, stalls
// or closes the connection. Keep-alive connections carry several requests.

#include "wizz/core/http.hpp"

#include <winsock2.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wizz::test {

struct ReceivedRequest {
    std::string method;
    std::string target;          ///< request-target as sent: "/path?q", or absolute form through a proxy
    wizz::http::Headers headers; ///< as received, in wire order
    std::string body;
    std::size_t connection = 0;  ///< 1-based number of the connection that carried it

    /// First header with this name (ASCII case-insensitive).
    [[nodiscard]] std::optional<std::string> header(std::string_view name) const;
    [[nodiscard]] std::size_t header_count(std::string_view name) const;
};

/// The connection a request arrived on, as seen by the handler.
class LoopbackConnection {
public:
    LoopbackConnection(SOCKET socket, std::stop_token stop) : socket_(socket), stop_(std::move(stop)) {}

    /// Writes raw bytes (errors are ignored: the client may already be gone).
    void send(std::string_view bytes);
    /// Blocks until the client closes the connection or the server shuts down.
    void wait_for_close();
    /// Closes the connection once the handler returns (FIN after what was sent).
    void close_after_response() noexcept { closing_ = true; }

    [[nodiscard]] bool closing() const noexcept { return closing_; }
    /// Appends received bytes to `into`: the count, 0 when the peer closed, nullopt on
    /// shutdown or error.
    [[nodiscard]] std::optional<std::size_t> receive(std::string& into);

private:
    SOCKET socket_;
    std::stop_token stop_;
    bool closing_ = false;
};

class LoopbackServer {
public:
    using Handler = std::function<void(const ReceivedRequest&, LoopbackConnection&)>;

    /// Starts listening at once. `handler` runs on connection threads, possibly concurrently.
    explicit LoopbackServer(Handler handler);
    /// Stops accepting, wakes stalled handlers and joins every thread.
    ~LoopbackServer();
    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    /// "http://127.0.0.1:<port>" + `path`.
    [[nodiscard]] std::string url(std::string_view path = "/") const;
    [[nodiscard]] std::vector<ReceivedRequest> requests() const;
    [[nodiscard]] std::size_t connection_count() const;

private:
    class WinsockSession {
    public:
        WinsockSession();
        ~WinsockSession();
        WinsockSession(const WinsockSession&) = delete;
        WinsockSession& operator=(const WinsockSession&) = delete;
    };

    void accept_loop(const std::stop_token& stop);
    void serve(SOCKET socket, std::size_t number, const std::stop_token& stop);

    WinsockSession winsock_;
    Handler handler_;
    SOCKET listener_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    mutable std::mutex mutex_; // guards the members below
    std::vector<ReceivedRequest> requests_;
    std::size_t connections_ = 0;
    std::vector<std::jthread> workers_;
    std::jthread acceptor_; // last: starts after everything above exists
};

/// "HTTP/1.1 <status> <reason>" + `headers` + "Content-Length" + blank line + `body`.
[[nodiscard]] std::string http_response(int status, const wizz::http::Headers& headers = {},
                                        std::string_view body = {});

/// A port on 127.0.0.1 with no listener (bound once, then released).
[[nodiscard]] std::uint16_t unused_loopback_port();

} // namespace wizz::test
