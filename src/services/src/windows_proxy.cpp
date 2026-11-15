#include "wizz/services/windows_proxy.hpp"

#include "win32_util.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <optional>

namespace wizz::http {
namespace {

[[nodiscard]] constexpr char ascii_lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool iequals_ascii(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](char x, char y) { return ascii_lower(x) == ascii_lower(y); });
}

[[nodiscard]] constexpr bool istarts_with_ascii(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() && iequals_ascii(text.substr(0, prefix.size()), prefix);
}

[[nodiscard]] constexpr std::string_view strip_brackets(std::string_view host) noexcept
{
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        return host.substr(1, host.size() - 2);
    }
    return host;
}

/// ASCII case-insensitive match where '*' stands for any run of characters.
[[nodiscard]] bool wildcard_match(std::string_view pattern, std::string_view text) noexcept
{
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resume = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = t;
        } else if (p < pattern.size() && ascii_lower(pattern[p]) == ascii_lower(text[t])) {
            ++p;
            ++t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

/// Calls `visit` for every non-empty token of `text` delimited by any of `separators`.
template <class Visit>
void for_each_token(std::string_view text, std::string_view separators, Visit&& visit)
{
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find_first_of(separators, start), text.size());
        if (end > start) {
            visit(text.substr(start, end - start));
        }
        start = end + 1;
    }
}

/// Four '.'-separated decimal parts, each 0–255 (an IPv4 literal).
[[nodiscard]] bool is_dotted_quad(std::string_view host) noexcept
{
    const auto is_octet = [](std::string_view part) {
        if (part.size() > 3) {
            return false;
        }
        int value = 0;
        for (const char c : part) {
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10 + (c - '0');
        }
        return value <= 255;
    };
    int parts = 0;
    bool valid = !host.starts_with('.') && !host.ends_with('.') && !host.contains("..");
    for_each_token(host, ".", [&](std::string_view part) {
        ++parts;
        valid = valid && is_octet(part);
    });
    return valid && parts == 4;
}

} // namespace

WindowsProxyConfig read_windows_proxy_config()
{
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG raw{};
    if (::WinHttpGetIEProxyConfigForCurrentUser(&raw) == FALSE) {
        return {};
    }
    const win32::GlobalString auto_config_url{raw.lpszAutoConfigUrl};
    const win32::GlobalString proxy{raw.lpszProxy};
    const win32::GlobalString bypass{raw.lpszProxyBypass};
    const auto text = [](const win32::GlobalString& value) {
        return value ? win32::to_utf8(value.get()) : std::string{};
    };
    return WindowsProxyConfig{
        .auto_detect = raw.fAutoDetect != FALSE,
        .auto_config_url = text(auto_config_url),
        .proxy = text(proxy),
        .bypass = text(bypass),
    };
}

bool environment_proxy_configured()
{
    for (const wchar_t* name : {L"http_proxy", L"https_proxy", L"all_proxy"}) {
        // The return value counts the terminating null, so a set, non-empty variable gives > 1.
        if (::GetEnvironmentVariableW(name, nullptr, 0) > 1) {
            return true;
        }
    }
    return false;
}

std::string select_manual_proxy(std::string_view proxy_list, std::string_view scheme)
{
    std::optional<std::string_view> generic;
    std::optional<std::string_view> specific;
    for_each_token(proxy_list, "; \t\r\n", [&](std::string_view entry) {
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            generic = generic.value_or(entry);
        } else if (!specific && iequals_ascii(entry.substr(0, equals), scheme)) {
            specific = entry.substr(equals + 1);
        }
    });
    const std::optional<std::string_view> chosen = specific ? specific : generic;
    if (!chosen || chosen->empty() || iequals_ascii(*chosen, "DIRECT")) {
        return {};
    }
    if (chosen->contains("://")) {
        return std::string{*chosen};
    }
    return "http://" + std::string{*chosen};
}

bool bypasses_proxy(std::string_view bypass_list, std::string_view host)
{
    const std::string_view bare_host = strip_brackets(host);
    bool bypassed = false;
    for_each_token(bypass_list, ";", [&](std::string_view entry) {
        entry.remove_prefix(std::min(entry.find_first_not_of(' '), entry.size()));
        if (istarts_with_ascii(entry, "http://")) {
            entry.remove_prefix(7);
        } else if (istarts_with_ascii(entry, "https://")) {
            entry.remove_prefix(8);
        }
        if (entry.starts_with('[')) {
            entry.remove_prefix(1);
        }
        entry = entry.substr(0, entry.find_first_of(" ]"));
        if (entry.empty()) {
            return;
        }
        if (iequals_ascii(entry, "<local>")) {
            bypassed = bypassed || is_loopback_host(bare_host) ||
                       (!bare_host.contains('.') && !bare_host.contains(':'));
        } else {
            bypassed = bypassed || wildcard_match(entry, bare_host);
        }
    });
    return bypassed;
}

bool is_loopback_host(std::string_view host)
{
    const std::string_view bare = strip_brackets(host);
    return iequals_ascii(bare, "localhost") || bare == "::1" || (bare.starts_with("127.") && is_dotted_quad(bare));
}

} // namespace wizz::http
