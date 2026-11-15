#pragma once
// Colours of the result grids (01 §8.1, §8.3, §9.1). The named system colours
// are the WinForms SystemColors the predecessor used (GetSysColor), not Qt's
// palette, whose Highlight follows the Windows accent colour.

#include <QColor>

namespace wizz::app::grid_colors {

/// `SystemColors.GrayText` (COLOR_GRAYTEXT): unavailable rows and the summary labels.
[[nodiscard]] QColor gray_text();
/// `SystemColors.Highlight` (COLOR_HIGHLIGHT): the selected row's background.
[[nodiscard]] QColor highlight();
/// `SystemColors.HighlightText` (COLOR_HIGHLIGHTTEXT): the selected row's text.
[[nodiscard]] QColor highlight_text();

/// Cheapest rows (01 §8.3): BackColor and SelectionBackColor.
inline constexpr QColor kCheapestBack{220, 245, 220};
inline constexpr QColor kCheapestSelectionBack{40, 140, 70};

} // namespace wizz::app::grid_colors
