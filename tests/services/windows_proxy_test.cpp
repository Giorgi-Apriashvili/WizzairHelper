// Proxy selection rules (04 §2.2 "Proxy", 06 §3): WinINet proxy and bypass lists as
// .NET's HttpWindowsProxy reads them.

#include "wizz/services/windows_proxy.hpp"

#include <gtest/gtest.h>

namespace {

using wizz::http::bypasses_proxy;
using wizz::http::is_loopback_host;
using wizz::http::select_manual_proxy;

TEST(WindowsProxy, Plain_proxy_applies_to_http_and_https)
{
    EXPECT_EQ(select_manual_proxy("proxy.corp:8080", "http"), "http://proxy.corp:8080");
    EXPECT_EQ(select_manual_proxy("proxy.corp:8080", "https"), "http://proxy.corp:8080");
}

TEST(WindowsProxy, Scheme_entries_win_over_a_plain_entry)
{
    constexpr const char* kList = "http=web:80;https=secure:443";
    EXPECT_EQ(select_manual_proxy(kList, "http"), "http://web:80");
    EXPECT_EQ(select_manual_proxy(kList, "https"), "http://secure:443");
    EXPECT_EQ(select_manual_proxy("https=secure:443;any:3128", "http"), "http://any:3128");
    EXPECT_EQ(select_manual_proxy("any:3128 https=secure:443", "https"), "http://secure:443");
    EXPECT_EQ(select_manual_proxy("HTTP=web:80", "http"), "http://web:80");
}

TEST(WindowsProxy, No_entry_for_the_scheme_means_direct)
{
    EXPECT_EQ(select_manual_proxy("http=web:80", "https"), "");
    EXPECT_EQ(select_manual_proxy("socks=socks:1080", "http"), "");
    EXPECT_EQ(select_manual_proxy("", "http"), "");
    EXPECT_EQ(select_manual_proxy(" ; ", "http"), "");
    EXPECT_EQ(select_manual_proxy("DIRECT", "http"), "");
}

TEST(WindowsProxy, First_plain_entry_is_used_and_explicit_schemes_are_kept)
{
    EXPECT_EQ(select_manual_proxy("first:1;second:2", "http"), "http://first:1");
    EXPECT_EQ(select_manual_proxy("first:1 second:2", "https"), "http://first:1");
    EXPECT_EQ(select_manual_proxy("http://explicit:8080", "https"), "http://explicit:8080");
    EXPECT_EQ(select_manual_proxy("https=https://tls-proxy:443", "https"), "https://tls-proxy:443");
}

TEST(WindowsProxy, Local_bypass_matches_plain_host_names_and_loopback)
{
    EXPECT_TRUE(bypasses_proxy("<local>", "intranet"));
    EXPECT_TRUE(bypasses_proxy("<LOCAL>", "localhost"));
    EXPECT_TRUE(bypasses_proxy("<local>", "127.0.0.1"));
    EXPECT_TRUE(bypasses_proxy("<local>", "[::1]"));
    EXPECT_FALSE(bypasses_proxy("<local>", "be.wizzair.com"));
    EXPECT_FALSE(bypasses_proxy("<local>", "10.0.0.1"));
    EXPECT_FALSE(bypasses_proxy("<local>", "[fe80::1]"));
}

TEST(WindowsProxy, Wildcard_bypass_entries_are_case_insensitive)
{
    EXPECT_TRUE(bypasses_proxy("*.example.com", "api.example.com"));
    EXPECT_TRUE(bypasses_proxy("*.EXAMPLE.com", "Api.Example.COM"));
    EXPECT_FALSE(bypasses_proxy("*.example.com", "example.com"));
    EXPECT_TRUE(bypasses_proxy("10.*", "10.1.2.3"));
    EXPECT_FALSE(bypasses_proxy("10.*", "110.1.2.3"));
    EXPECT_TRUE(bypasses_proxy("*wizz*", "be.wizzair.com"));
    EXPECT_TRUE(bypasses_proxy("be.wizzair.com", "be.wizzair.com"));
    EXPECT_FALSE(bypasses_proxy("be.wizzair.com", "www.wizzair.com"));
}

TEST(WindowsProxy, Bypass_list_parsing_follows_dotnet)
{
    EXPECT_TRUE(bypasses_proxy("a.test; b.test ;c.test", "b.test"));
    EXPECT_TRUE(bypasses_proxy("http://*.corp;https://intra", "intra"));
    EXPECT_TRUE(bypasses_proxy("[fe80::1]", "[fe80::1]"));
    EXPECT_FALSE(bypasses_proxy("a.test b.test;c.test", "b.test")); // an entry ends at a space
    EXPECT_FALSE(bypasses_proxy("", "anything.test"));
    EXPECT_FALSE(bypasses_proxy(";;", "anything.test"));
}

TEST(WindowsProxy, Loopback_hosts)
{
    for (const char* host : {"localhost", "LOCALHOST", "127.0.0.1", "127.255.255.254", "::1", "[::1]"}) {
        EXPECT_TRUE(is_loopback_host(host)) << host;
    }
    for (const char* host : {"localhost.test", "128.0.0.1", "127.0.0", "127.0.0.256", "127.0.0.1.5", "127..0.1",
                             "1127.0.0.1", "::2", "wizz.test", ""}) {
        EXPECT_FALSE(is_loopback_host(host)) << host;
    }
}

} // namespace
