#pragma once
// Proxy selection rules for the HTTP client (04 §2.2 "Proxy", 06 §3 "honour configured
// Windows proxies"). Like .NET's default proxy on Windows: the http_proxy /
// https_proxy / all_proxy environment variables win when set; otherwise the user's
// Internet Options apply (automatic detection or setup script first, then the manual
// proxy with its bypass list). These are the pure parts; CurlTransport applies them.

#include <string>
#include <string_view>

namespace wizz::http {

/// The current user's Internet Options proxy settings
/// (WinHttpGetIEProxyConfigForCurrentUser), as UTF-8.
struct WindowsProxyConfig {
    bool auto_detect = false;    ///< "Automatically detect settings" (WPAD)
    std::string auto_config_url; ///< "Use setup script" address (PAC); empty when unset
    std::string proxy;           ///< manual proxy list: "host:port" or "http=host:port;https=host:port"
    std::string bypass;          ///< manual bypass list, e.g. "<local>;*.example.com;10.*"

    friend bool operator==(const WindowsProxyConfig&, const WindowsProxyConfig&) = default;
};

/// Reads the current user's settings; a direct (empty) configuration when they
/// cannot be read.
[[nodiscard]] WindowsProxyConfig read_windows_proxy_config();

/// True when http_proxy, https_proxy or all_proxy is set (environment names are
/// case-insensitive on Windows). Such variables take precedence over the Windows
/// settings and libcurl applies them itself, together with no_proxy.
[[nodiscard]] bool environment_proxy_configured();

/// The proxy URL a WinINet proxy list assigns to `scheme` ("http" or "https"). Entries
/// are separated by ';' or white space; "scheme=address" applies to that scheme and
/// wins over a plain "address", which applies to every scheme. "http://" is prefixed
/// when the address names no scheme. Empty when the list has no proxy for the scheme
/// or the chosen entry is "DIRECT".
[[nodiscard]] std::string select_manual_proxy(std::string_view proxy_list, std::string_view scheme);

/// True when `host` matches the WinINet bypass list, as .NET's HttpWindowsProxy reads
/// it: ';'-separated entries, an optional http:// or https:// prefix and IPv6 brackets
/// are ignored, '*' is a wildcard, matching is ASCII case-insensitive. "<local>"
/// matches loopback hosts and host names without a dot.
[[nodiscard]] bool bypasses_proxy(std::string_view bypass_list, std::string_view host);

/// True for "localhost", 127.0.0.0/8 and ::1 (with or without brackets). Loopback
/// requests never use a proxy.
[[nodiscard]] bool is_loopback_host(std::string_view host);

} // namespace wizz::http
