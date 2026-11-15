#pragma once
// Win32 window bounds as WinForms sees them (01 §4.1, §13). A Form's Size and
// Bounds are the whole window rectangle, the invisible resize borders of
// Windows 10/11 included; WindowWidth / WindowHeight in settings.json are that
// size in device pixels. Qt sizes the client area and reports only the visible
// frame (the title bar), so the main window converts with these helpers.

#include <QMargins>
#include <QSize>

#include <optional>

class QWidget;

namespace wizz::app {

/// The full Win32 frame (client to window rectangle: borders, invisible resize borders,
/// title bar) of `window`'s native window at `device_pixel_ratio`, in device pixels
/// (AdjustWindowRectExForDpi). nullopt without a native Win32 window (the "minimal" /
/// "offscreen" platforms of the tests).
[[nodiscard]] std::optional<QMargins> native_frame_margins(const QWidget& window, double device_pixel_ratio);

/// The normal-state window size of `window` in device pixels — GetWindowPlacement's
/// rcNormalPosition, i.e. WinForms' RestoreBounds, also while maximised or minimised.
/// nullopt without a native Win32 window.
[[nodiscard]] std::optional<QSize> native_normal_outer_size(const QWidget& window);

} // namespace wizz::app
