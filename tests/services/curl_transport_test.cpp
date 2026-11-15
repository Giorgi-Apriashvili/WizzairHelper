// CurlTransport against a scripted loopback server (04 §2, §11.2; 06 §3). No test
// contacts anything but 127.0.0.1: proxied requests name a reserved .test host that
// only the loopback proxy ever sees.

#include "services/loopback_server.hpp"

#include "wizz/core/cancellation.hpp"
#include "wizz/services/curl_transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::http::CurlTransport;
using wizz::http::Headers;
using wizz::http::Request;
using wizz::http::Response;
using wizz::http::WindowsProxyConfig;
using wizz::test::http_response;
using wizz::test::LoopbackConnection;
using wizz::test::LoopbackServer;
using wizz::test::ReceivedRequest;

// 04 §2.3 wire capture.
constexpr const char* kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36";
constexpr const char* kTimetableBody =
    R"({"flightList":[{"departureStation":"BUD","arrivalStation":"LTN","from":"2026-11-05","to":"2026-11-07"}],"priceType":"regular","adultCount":1,"childCount":0,"infantCount":0})";
constexpr const char* kJson = "application/json; charset=utf-8";

Headers api_headers()
{
    return {{"User-Agent", kUserAgent},
            {"Accept", "application/json, text/plain, */*"},
            {"Accept-Language", "en-GB,en;q=0.9"},
            {"Origin", "https://www.wizzair.com"},
            {"Referer", "https://www.wizzair.com/en-gb"}};
}

/// A direct-connection transport, independent of the machine's proxy settings.
CurlTransport direct_transport()
{
    return CurlTransport{WindowsProxyConfig{}};
}

Request get(std::string url, Headers headers = {})
{
    return Request{.method = "GET", .url = std::move(url), .headers = std::move(headers)};
}

Request post(std::string url, std::string body, Headers headers = {})
{
    return Request{.method = "POST",
                   .url = std::move(url),
                   .headers = std::move(headers),
                   .body = std::move(body),
                   .content_type = kJson};
}

LoopbackServer::Handler respond_with(std::string raw_response)
{
    return [raw_response = std::move(raw_response)](const ReceivedRequest&, LoopbackConnection& connection) {
        connection.send(raw_response);
    };
}

bool wait_until(const auto& condition, std::chrono::milliseconds limit = 5s)
{
    const auto give_up = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > give_up) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// --- minimal encoders with stored (uncompressed) blocks: valid streams for libcurl to decode ---

void put_le16(std::string& out, std::uint32_t value)
{
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void put_le32(std::string& out, std::uint32_t value)
{
    put_le16(out, value & 0xFFFF);
    put_le16(out, value >> 16);
}

std::uint32_t crc32(std::string_view data)
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char c : data) {
        crc ^= static_cast<std::uint8_t>(c);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

std::uint32_t adler32(std::string_view data)
{
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const char c : data) {
        a = (a + static_cast<std::uint8_t>(c)) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

/// RFC 1951 stream of stored blocks.
std::string deflate_stored(std::string_view data)
{
    std::string out;
    for (;;) {
        const std::size_t length = std::min<std::size_t>(data.size(), 0xFFFF);
        const bool final = length == data.size();
        out.push_back(final ? '\x01' : '\x00');
        put_le16(out, static_cast<std::uint32_t>(length));
        put_le16(out, static_cast<std::uint32_t>(~length & 0xFFFF));
        out.append(data.substr(0, length));
        data.remove_prefix(length);
        if (final) {
            return out;
        }
    }
}

/// RFC 1952 gzip member.
std::string gzip_stored(std::string_view data)
{
    std::string out{"\x1F\x8B\x08\x00\x00\x00\x00\x00\x00\xFF", 10};
    out += deflate_stored(data);
    put_le32(out, crc32(data));
    put_le32(out, static_cast<std::uint32_t>(data.size()));
    return out;
}

/// RFC 1950 zlib stream (HTTP "deflate").
std::string zlib_stored(std::string_view data)
{
    std::string out{"\x78\x01"};
    out += deflate_stored(data);
    const std::uint32_t checksum = adler32(data);
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((checksum >> shift) & 0xFF));
    }
    return out;
}

/// RFC 7932 brotli stream of uncompressed meta-blocks.
std::string brotli_stored(std::string_view data)
{
    std::string out;
    std::uint32_t bits = 0;
    int bit_count = 0;
    const auto put = [&](std::uint32_t value, int count) {
        for (int i = 0; i < count; ++i) {
            bits |= ((value >> i) & 1u) << bit_count;
            if (++bit_count == 8) {
                out.push_back(static_cast<char>(bits));
                bits = 0;
                bit_count = 0;
            }
        }
    };
    const auto align = [&] {
        if (bit_count > 0) {
            put(0, 8 - bit_count);
        }
    };
    put(0, 1); // WBITS = 16
    while (!data.empty()) {
        const std::size_t length = std::min<std::size_t>(data.size(), 0x10000);
        put(0, 1);                                         // ISLAST = 0
        put(0, 2);                                         // MNIBBLES = 4
        put(static_cast<std::uint32_t>(length - 1), 16);   // MLEN - 1
        put(1, 1);                                         // ISUNCOMPRESSED
        align();
        out.append(data.substr(0, length));
        data.remove_prefix(length);
    }
    put(1, 1); // ISLAST
    put(1, 1); // ISLASTEMPTY
    align();
    return out;
}

std::string sample_text(std::size_t size)
{
    std::string text;
    text.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        text.push_back(static_cast<char>('a' + (i * 7 + i / 13) % 26));
    }
    return text;
}

// --- build -------------------------------------------------------------------------

TEST(CurlTransport, Libcurl_uses_schannel_and_supports_brotli_and_gzip)
{
    const std::string version = CurlTransport::library_version();
    EXPECT_TRUE(version.contains("Schannel")) << version;
    EXPECT_TRUE(version.contains("brotli")) << version;
    EXPECT_TRUE(version.contains("zlib")) << version;
}

// --- requests on the wire (04 §2.3) ------------------------------------------------

TEST(CurlTransport, Api_get_sends_exactly_the_captured_headers_in_order)
{
    LoopbackServer server{respond_with(http_response(200, {{"Content-Type", kJson}}, R"({"cities":[]})"))};
    CurlTransport transport; // system proxy settings: loopback always connects directly

    const Response response =
        transport.send(get(server.url("/29.23.1/Api/asset/map?languageCode=en-gb"), api_headers()), {});

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, R"({"cities":[]})");
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].method, "GET");
    EXPECT_EQ(requests[0].target, "/29.23.1/Api/asset/map?languageCode=en-gb");
    const Headers expected{{"Host", std::format("127.0.0.1:{}", server.port())},
                           {"User-Agent", kUserAgent},
                           {"Accept", "application/json, text/plain, */*"},
                           {"Accept-Language", "en-GB,en;q=0.9"},
                           {"Origin", "https://www.wizzair.com"},
                           {"Referer", "https://www.wizzair.com/en-gb"},
                           {"Accept-Encoding", "gzip, deflate, br"}};
    EXPECT_EQ(requests[0].headers, expected);
    EXPECT_EQ(requests[0].body, "");
}

TEST(CurlTransport, Api_post_sends_the_utf8_body_with_content_type_and_length_172)
{
    LoopbackServer server{respond_with(http_response(200, {{"Content-Type", kJson}}, R"({"outboundFlights":[]})"))};
    CurlTransport transport = direct_transport();

    const Response response = transport.send(post(server.url("/29.23.1/Api/search/timetableV2"), kTimetableBody,
                                                  api_headers()),
                                             {});

    EXPECT_EQ(response.status, 200);
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].method, "POST");
    EXPECT_EQ(requests[0].target, "/29.23.1/Api/search/timetableV2");
    EXPECT_EQ(requests[0].body, kTimetableBody);
    EXPECT_EQ(requests[0].body.size(), 172u);
    const Headers expected{{"Host", std::format("127.0.0.1:{}", server.port())},
                           {"User-Agent", kUserAgent},
                           {"Accept", "application/json, text/plain, */*"},
                           {"Accept-Language", "en-GB,en;q=0.9"},
                           {"Origin", "https://www.wizzair.com"},
                           {"Referer", "https://www.wizzair.com/en-gb"},
                           {"Accept-Encoding", "gzip, deflate, br"},
                           {"Content-Type", "application/json; charset=utf-8"},
                           {"Content-Length", "172"}};
    EXPECT_EQ(requests[0].headers, expected); // no Expect, Transfer-Encoding, Connection or Cookie
}

TEST(CurlTransport, Discovery_get_has_no_origin_referer_or_default_headers)
{
    LoopbackServer server{respond_with(http_response(200, {{"Content-Type", "text/html; charset=utf-8"}}, "<html/>"))};
    CurlTransport transport = direct_transport();

    const Headers discovery{{"User-Agent", kUserAgent},
                            {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                            {"Accept-Language", "en-GB,en;q=0.9"}};
    static_cast<void>(transport.send(get(server.url("/en-gb"), discovery), {}));
    static_cast<void>(transport.send(get(server.url("/bare")), {}));

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    const std::string host = std::format("127.0.0.1:{}", server.port());
    EXPECT_EQ(requests[0].headers, (Headers{{"Host", host},
                                            {"User-Agent", kUserAgent},
                                            {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                                            {"Accept-Language", "en-GB,en;q=0.9"},
                                            {"Accept-Encoding", "gzip, deflate, br"}}));
    // Without caller headers libcurl adds no User-Agent and no "Accept: */*".
    EXPECT_EQ(requests[1].headers, (Headers{{"Host", host}, {"Accept-Encoding", "gzip, deflate, br"}}));
}

TEST(CurlTransport, Post_without_body_or_content_type_sends_an_empty_body)
{
    LoopbackServer server{respond_with(http_response(200))};
    CurlTransport transport = direct_transport();

    static_cast<void>(transport.send(Request{.method = "POST", .url = server.url("/empty")}, {}));

    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].header("Content-Length"), "0");
    EXPECT_EQ(requests[0].header_count("Content-Type"), 0u);
}

TEST(CurlTransport, Rejects_unsupported_methods_and_header_injection)
{
    CurlTransport transport = direct_transport();
    EXPECT_THROW(static_cast<void>(transport.send(Request{.method = "PUT", .url = "http://127.0.0.1:1/"}, {})),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(
                     transport.send(get("http://127.0.0.1:1/", {{"X-Evil", "a\r\nInjected: yes"}}), {})),
                 std::invalid_argument);
}

// --- responses ---------------------------------------------------------------------

TEST(CurlTransport, Response_headers_keep_wire_order_and_content_type_is_separate)
{
    // Kasada shape (PR §6, CT-API-07): 429, x-kpsdk-* headers, empty text/plain body.
    LoopbackServer server{respond_with(http_response(429,
                                                     {{"x-kpsdk-ct", "abc"},
                                                      {"X-Cache", "Error from cloudfront"},
                                                      {"Set-Cookie", "ak_bm_a=1"},
                                                      {"Set-Cookie", "ak_bm_b=2"},
                                                      {"Content-Type", "text/plain; charset=utf-8"}}))};
    CurlTransport transport = direct_transport();

    const Response response = transport.send(get(server.url("/Api/search/timetableV2")), {});

    EXPECT_EQ(response.status, 429);
    EXPECT_EQ(response.content_type, "text/plain; charset=utf-8");
    EXPECT_EQ(response.body, "");
    EXPECT_EQ(response.headers, (Headers{{"x-kpsdk-ct", "abc"},
                                         {"X-Cache", "Error from cloudfront"},
                                         {"Set-Cookie", "ak_bm_a=1"},
                                         {"Set-Cookie", "ak_bm_b=2"},
                                         {"Content-Length", "0"}}));
}

TEST(CurlTransport, Error_statuses_are_responses_and_a_missing_content_type_is_nullopt)
{
    LoopbackServer server{respond_with(http_response(500))};
    CurlTransport transport = direct_transport();

    const Response response = transport.send(get(server.url("/")), {});

    EXPECT_EQ(response.status, 500);
    EXPECT_FALSE(response.content_type.has_value());
    EXPECT_EQ(response.body, "");
}

TEST(CurlTransport, Informational_responses_are_dropped)
{
    LoopbackServer server{respond_with("HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n" +
                                       http_response(200, {{"X-Final", "yes"}}, "ok"))};
    CurlTransport transport = direct_transport();

    const Response response = transport.send(get(server.url("/")), {});

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "ok");
    EXPECT_EQ(response.headers, (Headers{{"X-Final", "yes"}, {"Content-Length", "2"}}));
}

TEST(CurlTransport, Gzip_deflate_and_brotli_bodies_are_decoded)
{
    const std::string text = sample_text(200'000);
    LoopbackServer server{[&](const ReceivedRequest& request, LoopbackConnection& connection) {
        const std::string encoding = request.target.substr(1);
        const std::string encoded = encoding == "gzip"      ? gzip_stored(text)
                                    : encoding == "deflate" ? zlib_stored(text)
                                                            : brotli_stored(text);
        connection.send(http_response(200, {{"Content-Type", kJson}, {"Content-Encoding", encoding}}, encoded));
    }};
    CurlTransport transport = direct_transport();

    for (const char* encoding : {"gzip", "deflate", "br"}) {
        const Response response = transport.send(get(server.url(std::string{"/"} + encoding)), {});
        EXPECT_EQ(response.status, 200) << encoding;
        EXPECT_EQ(response.body.size(), text.size()) << encoding;
        EXPECT_TRUE(response.body == text) << encoding;
    }
}

TEST(CurlTransport, Keep_alive_connections_are_reused)
{
    LoopbackServer server{respond_with(http_response(200, {}, "x"))};
    CurlTransport transport = direct_transport();

    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(transport.send(get(server.url("/")), {}).body, "x");
    }
    EXPECT_EQ(server.requests().size(), 3u);
    EXPECT_EQ(server.connection_count(), 1u);
}

TEST(CurlTransport, Concurrent_sends_are_independent)
{
    LoopbackServer server{[](const ReceivedRequest& request, LoopbackConnection& connection) {
        connection.send(http_response(200, {}, request.target));
    }};
    CurlTransport transport = direct_transport();

    std::vector<std::future<bool>> results;
    for (int thread = 0; thread < 8; ++thread) {
        results.push_back(std::async(std::launch::async, [&, thread] {
            bool all_ok = true;
            for (int i = 0; i < 5; ++i) {
                const std::string path = std::format("/t{}/r{}", thread, i);
                all_ok = all_ok && transport.send(get(server.url(path)), {}).body == path;
            }
            return all_ok;
        }));
    }
    for (auto& result : results) {
        EXPECT_TRUE(result.get());
    }
    EXPECT_EQ(server.requests().size(), 40u);
}

// --- redirects (04 §2.2: .NET RedirectHandler) ----------------------------------------

TEST(CurlTransport, Post_answered_with_302_is_resent_as_get_without_content_or_cookies)
{
    LoopbackServer server{[](const ReceivedRequest& request, LoopbackConnection& connection) {
        if (request.target == "/start") {
            connection.send(http_response(302, {{"Location", "/next?step=2"}, {"Set-Cookie", "session=1"}}));
        } else {
            connection.send(http_response(200, {{"Content-Type", kJson}, {"X-Final", "yes"}}, "{}"));
        }
    }};
    CurlTransport transport = direct_transport();

    const Response response = transport.send(post(server.url("/start"), kTimetableBody, api_headers()), {});

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "{}");
    EXPECT_EQ(response.content_type, kJson);
    EXPECT_EQ(response.headers, (Headers{{"X-Final", "yes"}, {"Content-Length", "2"}})); // final response only
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[1].method, "GET");
    EXPECT_EQ(requests[1].target, "/next?step=2");
    EXPECT_EQ(requests[1].body, "");
    EXPECT_EQ(requests[1].header_count("Content-Type"), 0u);
    EXPECT_EQ(requests[1].header_count("Content-Length"), 0u);
    EXPECT_EQ(requests[1].header_count("Cookie"), 0u);
    EXPECT_EQ(requests[1].header("User-Agent"), kUserAgent);
    EXPECT_EQ(requests[1].header("Origin"), "https://www.wizzair.com");
}

TEST(CurlTransport, Redirect_method_rules_follow_dotnet)
{
    struct Case {
        int status;
        const char* expected_method;
        bool keeps_body;
    };
    for (const Case& c : {Case{300, "GET", false}, Case{301, "GET", false}, Case{303, "GET", false},
                          Case{307, "POST", true}, Case{308, "POST", true}}) {
        LoopbackServer server{[status = c.status](const ReceivedRequest& request, LoopbackConnection& connection) {
            connection.send(request.target == "/from" ? http_response(status, {{"Location", "/to"}})
                                                      : http_response(200, {}, "done"));
        }};
        CurlTransport transport = direct_transport();

        const Response response = transport.send(post(server.url("/from"), "{\"a\":1}"), {});

        EXPECT_EQ(response.status, 200) << c.status;
        const auto requests = server.requests();
        ASSERT_EQ(requests.size(), 2u) << c.status;
        EXPECT_EQ(requests[1].method, c.expected_method) << c.status;
        EXPECT_EQ(requests[1].body, c.keeps_body ? "{\"a\":1}" : "") << c.status;
        EXPECT_EQ(requests[1].header("Content-Type").has_value(), c.keeps_body) << c.status;
    }
}

TEST(CurlTransport, Absolute_redirect_to_another_port_is_followed)
{
    LoopbackServer target{respond_with(http_response(200, {}, "moved here"))};
    LoopbackServer origin{respond_with(http_response(301, {{"Location", target.url("/landing")}}))};
    CurlTransport transport = direct_transport();

    const Response response = transport.send(get(origin.url("/old")), {});

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "moved here");
    ASSERT_EQ(target.requests().size(), 1u);
    EXPECT_EQ(target.requests()[0].target, "/landing");
}

TEST(CurlTransport, Unfollowable_redirects_are_returned_to_the_caller)
{
    LoopbackServer server{[](const ReceivedRequest& request, LoopbackConnection& connection) {
        if (request.target == "/ftp") {
            connection.send(http_response(302, {{"Location", "ftp://files.test/x"}}, "see ftp"));
        } else {
            connection.send(http_response(302, {}, "no location"));
        }
    }};
    CurlTransport transport = direct_transport();

    const Response to_ftp = transport.send(get(server.url("/ftp")), {});
    EXPECT_EQ(to_ftp.status, 302);
    EXPECT_EQ(to_ftp.body, "see ftp");
    EXPECT_EQ(to_ftp.headers.front(), (std::pair<std::string, std::string>{"Location", "ftp://files.test/x"}));

    const Response without_location = transport.send(get(server.url("/nowhere")), {});
    EXPECT_EQ(without_location.status, 302);
    EXPECT_EQ(without_location.body, "no location");
    EXPECT_EQ(server.requests().size(), 2u);
}

TEST(CurlTransport, At_most_50_redirects_are_followed_then_the_3xx_is_returned)
{
    LoopbackServer server{[](const ReceivedRequest& request, LoopbackConnection& connection) {
        const int hop = std::stoi(request.target.substr(1));
        connection.send(http_response(302, {{"Location", std::format("/{}", hop + 1)}}));
    }};
    CurlTransport transport = direct_transport();

    const Response response = transport.send(get(server.url("/0")), {});

    EXPECT_EQ(response.status, 302);
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 51u); // the original request plus 50 redirects
    EXPECT_EQ(requests.back().target, "/50");
}

// --- failures, timeouts and cancellation (04 §11.2) ------------------------------------

TEST(CurlTransport, Connection_refused_is_a_transport_error)
{
    const std::uint16_t port = wizz::test::unused_loopback_port();
    CurlTransport transport = direct_transport();

    try {
        static_cast<void>(transport.send(get(std::format("http://127.0.0.1:{}/", port)), {}));
        FAIL() << "expected TransportError";
    } catch (const wizz::http::TransportError& error) {
        EXPECT_FALSE(std::string{error.what()}.empty());
    }
}

TEST(CurlTransport, Body_that_breaks_after_the_headers_is_a_transport_error_CT_SRCH_03)
{
    // 05 §1.2.4 BrokenStream: 200, application/json, the first 28 bytes, then the stream fails.
    LoopbackServer server{[](const ReceivedRequest&, LoopbackConnection& connection) {
        connection.send("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 400\r\n\r\n"
                        R"({"outboundFlights":[{"depart)");
        connection.close_after_response();
    }};
    CurlTransport transport = direct_transport();

    EXPECT_THROW(static_cast<void>(transport.send(post(server.url("/timetableV2"), kTimetableBody), {})),
                 wizz::http::TransportError);
}

TEST(CurlTransport, Server_that_sends_headers_then_stalls_times_out)
{
    LoopbackServer server{[](const ReceivedRequest&, LoopbackConnection& connection) {
        connection.send("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 100\r\n\r\n{\"partial");
        connection.wait_for_close();
    }};
    CurlTransport transport = direct_transport();
    Request request = get(server.url("/stall"));
    request.timeout = 300ms;

    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(static_cast<void>(transport.send(request, {})), wizz::http::TimeoutError);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, 250ms);
    EXPECT_LT(elapsed, 3s);
}

TEST(CurlTransport, Server_that_never_answers_times_out_P_10)
{
    LoopbackServer server{[](const ReceivedRequest&, LoopbackConnection& connection) { connection.wait_for_close(); }};
    CurlTransport transport = direct_transport();
    Request request = get(server.url("/silent"));
    request.timeout = 200ms;

    EXPECT_THROW(static_cast<void>(transport.send(request, {})), wizz::http::TimeoutError);
}

TEST(CurlTransport, Timeout_spans_redirect_hops)
{
    LoopbackServer server{[](const ReceivedRequest& request, LoopbackConnection& connection) {
        if (request.target == "/first") {
            std::this_thread::sleep_for(150ms);
            connection.send(http_response(302, {{"Location", "/second"}}));
        } else {
            std::this_thread::sleep_for(150ms);
            connection.send(http_response(200, {}, "late"));
        }
    }};
    CurlTransport transport = direct_transport();
    Request request = get(server.url("/first"));
    request.timeout = 250ms; // each hop fits, both together do not

    EXPECT_THROW(static_cast<void>(transport.send(request, {})), wizz::http::TimeoutError);
}

TEST(CurlTransport, Cancel_during_a_stalled_transfer_returns_within_300_ms)
{
    LoopbackServer server{[](const ReceivedRequest&, LoopbackConnection& connection) {
        connection.send("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n{");
        connection.wait_for_close();
    }};
    CurlTransport transport = direct_transport();
    std::stop_source source;

    auto pending = std::async(std::launch::async, [&] { return transport.send(get(server.url("/slow")), source.get_token()); });
    ASSERT_TRUE(wait_until([&] { return server.requests().size() == 1; }));
    std::this_thread::sleep_for(150ms); // well inside the transfer
    const auto cancelled_at = std::chrono::steady_clock::now();
    source.request_stop();

    EXPECT_THROW(static_cast<void>(pending.get()), wizz::OperationCancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - cancelled_at, 300ms);
}

TEST(CurlTransport, Cancelled_token_sends_nothing_and_wins_over_a_timeout)
{
    LoopbackServer server{respond_with(http_response(200))};
    CurlTransport transport = direct_transport();
    std::stop_source source;
    source.request_stop();
    Request request = get(server.url("/"));
    request.timeout = 0ms; // already expired as well

    EXPECT_THROW(static_cast<void>(transport.send(request, source.get_token())), wizz::OperationCancelled);
    EXPECT_TRUE(server.requests().empty());
}

// --- proxy discovery is part of the attempt (04 §11.2) -----------------------------------

/// Stands in for a slow WinHTTP proxy discovery: the lookup blocks until the gate opens
/// (or `limit` passes, so that a failing test cannot hang), then connects directly.
struct ProxyGate {
    std::mutex mutex;
    std::condition_variable opened;
    bool is_open = false;
    std::atomic<int> calls{0};

    void open()
    {
        {
            const std::scoped_lock lock{mutex};
            is_open = true;
        }
        opened.notify_all();
    }
};

CurlTransport::ProxySelector gated_selector(const std::shared_ptr<ProxyGate>& gate,
                                            std::chrono::milliseconds limit = 3s)
{
    return [gate, limit](const std::string&) -> std::optional<std::string> {
        ++gate->calls;
        std::unique_lock lock{gate->mutex};
        gate->opened.wait_for(lock, limit, [&] { return gate->is_open; });
        return std::string{};
    };
}

/// Opens the gate when a test ends; declared after the transport, so it runs before the
/// transport waits for an abandoned lookup.
class GateOpener {
public:
    explicit GateOpener(std::shared_ptr<ProxyGate> gate) : gate_(std::move(gate)) {}
    ~GateOpener() { gate_->open(); }
    GateOpener(const GateOpener&) = delete;
    GateOpener& operator=(const GateOpener&) = delete;

private:
    std::shared_ptr<ProxyGate> gate_;
};

TEST(CurlTransport, Proxy_discovery_counts_against_the_request_timeout)
{
    LoopbackServer server{respond_with(http_response(200, {}, "too late"))};
    const auto gate = std::make_shared<ProxyGate>();
    CurlTransport transport{gated_selector(gate)};
    const GateOpener open_at_end{gate};
    Request request = get(server.url("/"));
    request.timeout = 200ms;

    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(static_cast<void>(transport.send(request, {})), wizz::http::TimeoutError);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, 150ms);
    EXPECT_LT(elapsed, 1s); // the wait ends at the deadline, not when the lookup returns
    EXPECT_TRUE(server.requests().empty());
}

TEST(CurlTransport, Proxy_discovery_time_is_taken_from_the_transfer_budget)
{
    LoopbackServer server{[](const ReceivedRequest&, LoopbackConnection& connection) {
        std::this_thread::sleep_for(250ms);
        connection.send(http_response(200, {}, "slow"));
    }};
    CurlTransport transport{[](const std::string&) -> std::optional<std::string> {
        std::this_thread::sleep_for(200ms);
        return std::string{};
    }};
    Request request = get(server.url("/"));
    request.timeout = 350ms; // the lookup and the answer each fit, together they do not

    EXPECT_THROW(static_cast<void>(transport.send(request, {})), wizz::http::TimeoutError);
}

TEST(CurlTransport, Cancel_during_proxy_discovery_returns_within_300_ms)
{
    LoopbackServer server{respond_with(http_response(200))};
    const auto gate = std::make_shared<ProxyGate>();
    CurlTransport transport{gated_selector(gate)};
    const GateOpener open_at_end{gate};
    std::stop_source source;

    auto pending = std::async(std::launch::async, [&] { return transport.send(get(server.url("/")), source.get_token()); });
    ASSERT_TRUE(wait_until([&] { return gate->calls.load() == 1; }));
    const auto cancelled_at = std::chrono::steady_clock::now();
    source.request_stop();

    EXPECT_THROW(static_cast<void>(pending.get()), wizz::OperationCancelled);
    EXPECT_LT(std::chrono::steady_clock::now() - cancelled_at, 300ms);
    EXPECT_TRUE(server.requests().empty());
}

TEST(CurlTransport, Proxy_selector_chooses_the_proxy_for_each_request)
{
    LoopbackServer proxy{[](const ReceivedRequest& request, LoopbackConnection& connection) {
        connection.send(http_response(200, {}, "proxied " + request.target));
    }};
    LoopbackServer server{respond_with(http_response(200, {}, "direct"))};
    CurlTransport transport{[port = proxy.port()](const std::string& url) -> std::optional<std::string> {
        return url.starts_with("http://wizz.test/") ? std::format("http://127.0.0.1:{}", port) : std::string{};
    }};

    EXPECT_EQ(transport.send(get("http://wizz.test/data?x=1"), {}).body, "proxied http://wizz.test/data?x=1");
    EXPECT_EQ(transport.send(get(server.url("/")), {}).body, "direct");
}

// --- proxies (06 §3; selection rules in windows_proxy_test.cpp) --------------------------

TEST(CurlTransport, Manual_windows_proxy_receives_the_absolute_request)
{
    LoopbackServer proxy{[](const ReceivedRequest& request, LoopbackConnection& connection) {
        connection.send(http_response(200, {}, "proxied " + request.target));
    }};
    CurlTransport transport{WindowsProxyConfig{.proxy = std::format("https=127.0.0.1:1;http=127.0.0.1:{}", proxy.port())}};

    const Response response = transport.send(get("http://wizz.test/data?x=1", api_headers()), {});

    EXPECT_EQ(response.body, "proxied http://wizz.test/data?x=1");
    const auto requests = proxy.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].header("Host"), "wizz.test");
    EXPECT_EQ(requests[0].header("User-Agent"), kUserAgent);
}

TEST(CurlTransport, Proxy_auto_config_script_is_evaluated_through_winhttp)
{
    std::atomic<std::uint16_t> proxy_port{0};
    LoopbackServer server{[&](const ReceivedRequest& request, LoopbackConnection& connection) {
        if (request.target == "/proxy.pac") {
            const std::string script =
                std::format("function FindProxyForURL(url, host) {{ return \"PROXY 127.0.0.1:{}\"; }}", proxy_port.load());
            connection.send(http_response(200, {{"Content-Type", "application/x-ns-proxy-autoconfig"}}, script));
        } else {
            connection.send(http_response(200, {}, "via pac " + request.target));
        }
    }};
    proxy_port = server.port();
    // If WinHTTP cannot run the script, the manual fallback is a refused loopback port,
    // so the .test host is never resolved.
    CurlTransport transport{WindowsProxyConfig{.auto_config_url = server.url("/proxy.pac"), .proxy = "127.0.0.1:1"}};

    const Response response = transport.send(get("http://wizz.test/pac"), {});

    EXPECT_EQ(response.body, "via pac http://wizz.test/pac");
}

TEST(CurlTransport, Loopback_hosts_bypass_a_configured_proxy)
{
    LoopbackServer server{respond_with(http_response(200, {}, "direct"))};
    CurlTransport transport{WindowsProxyConfig{.proxy = "127.0.0.1:1"}};

    EXPECT_EQ(transport.send(get(server.url("/")), {}).body, "direct");
}

} // namespace
