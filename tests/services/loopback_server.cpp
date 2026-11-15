#include "services/loopback_server.hpp"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <stdexcept>

// The test target links only core, services and GoogleTest; Winsock is needed here alone.
#pragma comment(lib, "ws2_32.lib")

namespace wizz::test {
namespace {

constexpr int kPollTimeoutMs = 20;

[[nodiscard]] bool iequals_ascii(std::string_view a, std::string_view b) noexcept
{
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() && std::ranges::equal(a, b, [&](char x, char y) { return lower(x) == lower(y); });
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

[[nodiscard]] std::string_view reason_phrase(int status) noexcept
{
    switch (status) {
    case 200: return "OK";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 404: return "Not Found";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    default: return "Status";
    }
}

/// Parses "METHOD target HTTP/1.1" plus header lines; nullopt when malformed.
[[nodiscard]] std::optional<ReceivedRequest> parse_head(std::string_view head)
{
    ReceivedRequest request;
    const auto line_end = head.find("\r\n");
    const std::string_view request_line = head.substr(0, line_end);
    const auto first_space = request_line.find(' ');
    const auto second_space = request_line.find(' ', first_space + 1);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
        return std::nullopt;
    }
    request.method = request_line.substr(0, first_space);
    request.target = request_line.substr(first_space + 1, second_space - first_space - 1);
    std::size_t start = line_end == std::string_view::npos ? head.size() : line_end + 2;
    while (start < head.size()) {
        const auto end = std::min(head.find("\r\n", start), head.size());
        const std::string_view line = head.substr(start, end - start);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        request.headers.emplace_back(std::string{line.substr(0, colon)}, std::string{trim(line.substr(colon + 1))});
        start = end + 2;
    }
    return request;
}

} // namespace

// --- ReceivedRequest -------------------------------------------------------------------

std::optional<std::string> ReceivedRequest::header(std::string_view name) const
{
    const auto match = std::ranges::find_if(headers, [&](const auto& field) { return iequals_ascii(field.first, name); });
    return match == headers.end() ? std::nullopt : std::optional<std::string>{match->second};
}

std::size_t ReceivedRequest::header_count(std::string_view name) const
{
    return static_cast<std::size_t>(
        std::ranges::count_if(headers, [&](const auto& field) { return iequals_ascii(field.first, name); }));
}

// --- LoopbackConnection ----------------------------------------------------------------

void LoopbackConnection::send(std::string_view bytes)
{
    while (!bytes.empty()) {
        const int chunk = static_cast<int>(std::min<std::size_t>(bytes.size(), 1u << 20));
        const int sent = ::send(socket_, bytes.data(), chunk, 0);
        if (sent <= 0) {
            return;
        }
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
}

std::optional<std::size_t> LoopbackConnection::receive(std::string& into)
{
    while (!stop_.stop_requested()) {
        WSAPOLLFD poll{.fd = socket_, .events = POLLRDNORM, .revents = 0};
        const int ready = ::WSAPoll(&poll, 1, kPollTimeoutMs);
        if (ready < 0) {
            return std::nullopt;
        }
        if (ready == 0) {
            continue;
        }
        std::array<char, 16 * 1024> chunk{};
        const int received = ::recv(socket_, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (received < 0) {
            return std::nullopt;
        }
        into.append(chunk.data(), static_cast<std::size_t>(received));
        return static_cast<std::size_t>(received);
    }
    return std::nullopt;
}

void LoopbackConnection::wait_for_close()
{
    std::string discarded;
    while (true) {
        const auto received = receive(discarded);
        if (!received || *received == 0) {
            return;
        }
        discarded.clear();
    }
}

// --- LoopbackServer --------------------------------------------------------------------

LoopbackServer::WinsockSession::WinsockSession()
{
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
}

LoopbackServer::WinsockSession::~WinsockSession()
{
    ::WSACleanup();
}

LoopbackServer::LoopbackServer(Handler handler) : handler_(std::move(handler))
{
    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET) {
        throw std::runtime_error("socket() failed");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int length = sizeof address;
    if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0 ||
        ::listen(listener_, SOMAXCONN) != 0 ||
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::closesocket(listener_);
        throw std::runtime_error("Could not listen on 127.0.0.1");
    }
    port_ = ::ntohs(address.sin_port);
    acceptor_ = std::jthread{[this](std::stop_token stop) { accept_loop(stop); }};
}

LoopbackServer::~LoopbackServer()
{
    acceptor_.request_stop();
    acceptor_.join();
    std::vector<std::jthread> workers;
    {
        const std::lock_guard lock(mutex_);
        workers.swap(workers_);
    }
    workers.clear(); // jthread: request_stop + join
    ::closesocket(listener_);
}

std::string LoopbackServer::url(std::string_view path) const
{
    return std::format("http://127.0.0.1:{}{}", port_, path);
}

std::vector<ReceivedRequest> LoopbackServer::requests() const
{
    const std::lock_guard lock(mutex_);
    return requests_;
}

std::size_t LoopbackServer::connection_count() const
{
    const std::lock_guard lock(mutex_);
    return connections_;
}

void LoopbackServer::accept_loop(const std::stop_token& stop)
{
    while (!stop.stop_requested()) {
        WSAPOLLFD poll{.fd = listener_, .events = POLLRDNORM, .revents = 0};
        if (::WSAPoll(&poll, 1, kPollTimeoutMs) <= 0) {
            continue;
        }
        const SOCKET client = ::accept(listener_, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            continue;
        }
        const std::lock_guard lock(mutex_);
        const std::size_t number = ++connections_;
        workers_.emplace_back([this, client, number](std::stop_token worker_stop) { serve(client, number, worker_stop); });
    }
}

void LoopbackServer::serve(SOCKET socket, std::size_t number, const std::stop_token& stop)
{
    LoopbackConnection connection{socket, stop};
    std::string pending;
    while (!connection.closing()) {
        std::size_t head_end = pending.find("\r\n\r\n");
        while (head_end == std::string::npos) {
            const auto received = connection.receive(pending);
            if (!received || *received == 0) {
                head_end = std::string::npos;
                break;
            }
            head_end = pending.find("\r\n\r\n");
        }
        if (head_end == std::string::npos) {
            break;
        }
        auto request = parse_head(std::string_view{pending}.substr(0, head_end));
        if (!request) {
            break;
        }
        pending.erase(0, head_end + 4);
        std::size_t body_length = 0;
        if (const auto content_length = request->header("Content-Length")) {
            std::from_chars(content_length->data(), content_length->data() + content_length->size(), body_length);
        }
        bool complete = true;
        while (pending.size() < body_length) {
            const auto received = connection.receive(pending);
            if (!received || *received == 0) {
                complete = false;
                break;
            }
        }
        if (!complete) {
            break;
        }
        request->body = pending.substr(0, body_length);
        pending.erase(0, body_length);
        request->connection = number;
        {
            const std::lock_guard lock(mutex_);
            requests_.push_back(*request);
        }
        handler_(*request, connection);
    }
    ::shutdown(socket, SD_SEND);
    ::closesocket(socket);
}

std::string http_response(int status, const wizz::http::Headers& headers, std::string_view body)
{
    std::string response = std::format("HTTP/1.1 {} {}\r\n", status, reason_phrase(status));
    for (const auto& [name, value] : headers) {
        response += std::format("{}: {}\r\n", name, value);
    }
    response += std::format("Content-Length: {}\r\n\r\n", body.size());
    response += body;
    return response;
}

std::uint16_t unused_loopback_port()
{
    const LoopbackServer probe{[](const ReceivedRequest&, LoopbackConnection&) {}};
    return probe.port();
}

} // namespace wizz::test
