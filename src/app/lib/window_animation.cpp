#include "window_animation.hpp"

#include <QGuiApplication>
#include <QString>
#include <QWidget>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wizz::app {

namespace {

/// A BOOL system parameter; `fallback` when it cannot be read.
[[nodiscard]] bool system_flag(UINT action, bool fallback)
{
    BOOL value = fallback ? TRUE : FALSE;
    if (!SystemParametersInfoW(action, 0, &value, 0)) {
        return fallback;
    }
    return value != FALSE;
}

/// DisallowAnimations under `root`'s DWM policy key is a non-zero DWORD.
[[nodiscard]] bool policy_set(HKEY root)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(root, L"Software\\Policies\\Microsoft\\Windows\\DWM", L"DisallowAnimations",
                                        RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value != 0;
}

} // namespace

WindowAnimationSettings read_window_animation_settings()
{
    WindowAnimationSettings settings;
    settings.client_area_animation = system_flag(SPI_GETCLIENTAREAANIMATION, true);
    ANIMATIONINFO info{};
    info.cbSize = sizeof(info);
    settings.minimize_animation =
        SystemParametersInfoW(SPI_GETANIMATION, sizeof(info), &info, 0) ? info.iMinAnimate != 0 : true;
    settings.policy_disallows = policy_set(HKEY_CURRENT_USER) || policy_set(HKEY_LOCAL_MACHINE);
    return settings;
}

bool opens_with_animation(const QWidget& window)
{
    const QWidget* top = window.window();
    if (QGuiApplication::platformName() != QStringLiteral("windows") || top->windowHandle() == nullptr) {
        return false;  // no native Win32 window: nothing for DWM to animate
    }
    return read_window_animation_settings().animate();
}

} // namespace wizz::app
