#pragma once
// Chooses the proxy for each request (04 §2.2 "Proxy", 06 §3), mirroring .NET's
// default proxy on Windows: environment variables, else the user's Internet Options —
// automatic detection / setup script through WinHTTP, falling back to the manual proxy
// and its bypass list.

#include "wizz/services/windows_proxy.hpp"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace wizz::http {

class ProxyResolver {
public:
    /// `fixed` nullopt: the process environment and the current user's Windows settings,
    /// both read now (like .NET, later changes are not picked up). Otherwise exactly
    /// `fixed`, ignoring the environment.
    explicit ProxyResolver(std::optional<WindowsProxyConfig> fixed);

    /// The CURLOPT_PROXY value for one request: nullopt leaves the choice to libcurl
    /// (environment variables), "" means a direct connection, anything else is a proxy URL.
    /// May block while WinHTTP detects or runs a setup script.
    [[nodiscard]] std::optional<std::string> proxy_for(const std::string& url, std::string_view scheme,
                                                       std::string_view host);

private:
    struct WinHttpCloser {
        void operator()(HINTERNET handle) const noexcept { ::WinHttpCloseHandle(handle); }
    };
    using WinHttpHandle = std::unique_ptr<void, WinHttpCloser>;

    /// Proxy from automatic detection or the setup script; nullopt when neither is
    /// configured or WinHTTP could not evaluate them (then the manual settings apply).
    [[nodiscard]] std::optional<std::string> automatic_proxy(const std::string& url, std::string_view scheme,
                                                             std::string_view host);

    bool use_environment_;
    WindowsProxyConfig config_;
    std::mutex mutex_; // guards the members below
    WinHttpHandle session_;
    std::optional<std::chrono::steady_clock::time_point> detection_failed_at_;
};

} // namespace wizz::http
