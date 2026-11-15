#pragma once
// Product identity shared by the window, dialogs and diagnostics (00-README, 06 §4).

#include <QString>

namespace wizz::app {

/// "WizzairHelper" — caption of every MessageBox and the product name.
[[nodiscard]] QString product_name();
/// Main window title (01 §4.1).
[[nodiscard]] QString main_window_title();

} // namespace wizz::app
