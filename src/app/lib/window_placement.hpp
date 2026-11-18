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

/// WinForms' Form.WindowState.
enum class WindowShowState { normal, minimized, maximized };

/// Form.UpdateWindowState: the state for GetWindowPlacement's `showCmd`.
[[nodiscard]] WindowShowState window_show_state(unsigned show_cmd) noexcept;

/// The size closing saves (01 §13): `(WindowState == Normal) ? Bounds : RestoreBounds`.
/// An Aero-snapped window is Normal, so its snapped size is saved, not the size before
/// the snap that RestoreBounds still holds.
[[nodiscard]] QSize bounds_to_save(WindowShowState state, QSize bounds, QSize restore_bounds) noexcept;

/// The native window as WinForms sees it, sizes in device pixels.
struct NativeBounds {
    WindowShowState state = WindowShowState::normal;  ///< WindowState (GetWindowPlacement's showCmd)
    QSize bounds;           ///< Bounds (GetWindowRect)
    QSize restore_bounds;   ///< RestoreBounds (rcNormalPosition)
};

/// The state and sizes of `window`'s native window; nullopt without a native Win32 window.
[[nodiscard]] std::optional<NativeBounds> native_bounds(const QWidget& window);

/// Turns the desktop window manager's transitions of `window`'s native window (the
/// animations Windows plays when it opens, closes, minimises or restores) off or back on
/// (DWMWA_TRANSITIONS_FORCEDISABLED). Returns whether DWM took the setting: false
/// without a native Win32 window.
bool set_window_transitions(const QWidget& window, bool enabled);

} // namespace wizz::app
