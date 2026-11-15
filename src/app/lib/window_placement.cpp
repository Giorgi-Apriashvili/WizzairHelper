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

std::optional<QSize> native_normal_outer_size(const QWidget& window)
{
    const HWND hwnd = native_window(window);
    if (hwnd == nullptr) {
        return std::nullopt;
    }
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!GetWindowPlacement(hwnd, &placement)) {
        return std::nullopt;
    }
    const RECT& normal = placement.rcNormalPosition;
    return QSize{normal.right - normal.left, normal.bottom - normal.top};
}

} // namespace wizz::app
