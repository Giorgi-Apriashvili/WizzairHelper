#pragma once
// Colours of the result grids (01 §8.1, §8.3, §9.1), prototype "layout"
// direction: they come from the light or dark theme (ui_style.hpp) instead of
// the WinForms SystemColors.

#include <QColor>

namespace wizz::app::grid_colors {

/// Muted text: unavailable rows and the summary labels.
[[nodiscard]] QColor gray_text();
/// The selected row's background: a soft tint of the accent.
[[nodiscard]] QColor highlight();
/// The selected row's text.
[[nodiscard]] QColor highlight_text();
/// Cheapest rows (01 §8.3): background, and background while selected.
[[nodiscard]] QColor cheapest_back();
[[nodiscard]] QColor cheapest_selection_back();

/// The WinForms colours of 01 §8.3, kept for reference (the theme replaces them).
inline constexpr QColor kCheapestBack{220, 245, 220};
inline constexpr QColor kCheapestSelectionBack{40, 140, 70};

} // namespace wizz::app::grid_colors
