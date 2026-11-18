#include "window_placement.hpp"

#include <QGuiApplication>
#include <QString>
#include <QWidget>

#include <cmath>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wizz::app {

namespace {

[[nodiscard]] HWND native_window(const QWidget& window)
{
    const QWidget* top = window.window();
    if (QGuiApplication::platformName() != QStringLiteral("windows") || top->windowHandle() == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<HWND>(top->winId());
}

} // namespace

WindowShowState window_show_state(unsigned show_cmd) noexcept
{
    switch (show_cmd) {
    case SW_SHOWMAXIMIZED:
        return WindowShowState::maximized;
    case SW_SHOWMINIMIZED:
    case SW_MINIMIZE:
    case SW_SHOWMINNOACTIVE:
        return WindowShowState::minimized;
    default:
        return WindowShowState::normal;
    }
}

QSize bounds_to_save(WindowShowState state, QSize bounds, QSize restore_bounds) noexcept
{
    return state == WindowShowState::normal ? bounds : restore_bounds;
}

std::optional<QMargins> native_frame_margins(const QWidget& window, double device_pixel_ratio)
{
    const HWND hwnd = native_window(window);
    if (hwnd == nullptr) {
        return std::nullopt;
    }
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    const auto dpi = static_cast<UINT>(std::lround(USER_DEFAULT_SCREEN_DPI * device_pixel_ratio));
    RECT rect{0, 0, 0, 0};
    if (!AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, dpi)) {
        return std::nullopt;
    }
    return QMargins{-rect.left, -rect.top, rect.right, rect.bottom};
}

std::optional<NativeBounds> native_bounds(const QWidget& window)
{
    const HWND hwnd = native_window(window);
    if (hwnd == nullptr) {
        return std::nullopt;
    }
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    RECT rect{};
    if (!GetWindowPlacement(hwnd, &placement) || !GetWindowRect(hwnd, &rect)) {
        return std::nullopt;
    }
    const RECT& normal = placement.rcNormalPosition;
    return NativeBounds{.state = window_show_state(placement.showCmd),
                        .bounds = QSize{rect.right - rect.left, rect.bottom - rect.top},
                        .restore_bounds = QSize{normal.right - normal.left, normal.bottom - normal.top}};
}

} // namespace wizz::app
