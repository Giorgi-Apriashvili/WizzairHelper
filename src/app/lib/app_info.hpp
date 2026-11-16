#pragma once
// Product identity shared by the window, dialogs and diagnostics (00-README, 06 §4).

#include <QIcon>
#include <QString>

#include <array>

namespace wizz::app {

/// "WizzairHelper" — caption of every MessageBox and the product name.
[[nodiscard]] QString product_name();
/// Main window title (01 §4.1).
[[nodiscard]] QString main_window_title();

/// The pixel sizes of the application icon (D-51): the .ico, the exe's icon group and the
/// window icon each hold exactly one image of each size.
inline constexpr std::array<int, 10> kAppIconSizes{16, 20, 24, 32, 40, 48, 64, 96, 128, 256};

/// The application icon: one embedded PNG per size of kAppIconSizes, drawn for that size
/// (the Qt resource :/icons/ of src/app/resources/icon, D-51). Application makes it the icon
/// of every window (title bar, Alt+Tab, taskbar button).
[[nodiscard]] QIcon application_icon();

} // namespace wizz::app
