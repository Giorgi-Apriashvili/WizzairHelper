#include "proxy_resolver.hpp"

#include "win32_util.hpp"

namespace wizz::http {
namespace {

/// After automatic detection failed, .NET skips it for two minutes
/// (WinInetProxyHelper.RecentAutoDetectionInterval).
constexpr std::chrono::minutes kDetectionRetryInterval{2};

} // namespace

ProxyResolver::ProxyResolver(std::optional<WindowsProxyConfig> fixed)
    : use_environment_(!fixed && environment_proxy_configured()),
      config_(fixed ? std::move(*fixed) : read_windows_proxy_config())
{
}

std::optional<std::string> ProxyResolver::proxy_for(const std::string& url, std::string_view scheme,
                                                    std::string_view host)
{
    if (is_loopback_host(host)) {
        return std::string{};
    }
    if (use_environment_) {
        return std::nullopt;
    }
    if (auto automatic = automatic_proxy(url, scheme, host)) {
        return automatic;
    }
    if (config_.proxy.empty() || bypasses_proxy(config_.bypass, host)) {
        return std::string{};
    }
    return select_manual_proxy(config_.proxy, scheme);
}

bool ProxyResolver::may_block(std::string_view host) const
{
    // Mirrors the order of proxy_for; config_ and use_environment_ never change.
    return !is_loopback_host(host) && !use_environment_ && (config_.auto_detect || !config_.auto_config_url.empty());
}

std::optional<std::string> ProxyResolver::automatic_proxy(const std::string& url, std::string_view scheme,
                                                          std::string_view host)
{
    const bool has_script = !config_.auto_config_url.empty();
    if (!config_.auto_detect && !has_script) {
        return std::nullopt;
    }
    const std::lock_guard lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const bool detect =
        config_.auto_detect && !(detection_failed_at_ && now - *detection_failed_at_ < kDetectionRetryInterval);
    if (!detect && !has_script) {
        return std::nullopt;
    }
    if (!session_) {
        session_.reset(::WinHttpOpen(L"WizzairHelper", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session_) {
            return std::nullopt;
        }
    }

    const std::wstring wide_url = win32::to_wide(url);
    const std::wstring script_url = win32::to_wide(config_.auto_config_url);
    WINHTTP_AUTOPROXY_OPTIONS options{};
    options.dwFlags = (detect ? WINHTTP_AUTOPROXY_AUTO_DETECT : 0u) | (has_script ? WINHTTP_AUTOPROXY_CONFIG_URL : 0u);
    options.dwAutoDetectFlags = detect ? WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A : 0u;
    options.lpszAutoConfigUrl = has_script ? script_url.c_str() : nullptr;

    WINHTTP_PROXY_INFO info{};
    BOOL found = ::WinHttpGetProxyForUrl(session_.get(), wide_url.c_str(), &options, &info);
    DWORD error = found ? ERROR_SUCCESS : ::GetLastError();
    if (!found && error == ERROR_WINHTTP_LOGIN_FAILURE) {
        // The script server asked for credentials: retry once with the signed-in user's.
        options.fAutoLogonIfChallenged = TRUE;
        found = ::WinHttpGetProxyForUrl(session_.get(), wide_url.c_str(), &options, &info);
        error = found ? ERROR_SUCCESS : ::GetLastError();
    }
    if (!found) {
        if (error == ERROR_WINHTTP_AUTODETECTION_FAILED) {
            detection_failed_at_ = now;
        }
        return std::nullopt;
    }
    const win32::GlobalString proxy{info.lpszProxy};
    const win32::GlobalString bypass{info.lpszProxyBypass};
    if (info.dwAccessType != WINHTTP_ACCESS_TYPE_NAMED_PROXY || !proxy) {
        return std::string{}; // the script answered DIRECT
    }
    if (bypass && bypasses_proxy(win32::to_utf8(bypass.get()), host)) {
        return std::string{};
    }
    return select_manual_proxy(win32::to_utf8(proxy.get()), scheme);
}

} // namespace wizz::http
