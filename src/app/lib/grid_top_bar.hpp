#pragma once
// The top bar of both result grids (01 §8.1, §9.1), prototype "layout"
// direction: one row inside the results card, padding (12, 8, 12, 8), its
// controls vertically centred and separated by their own margins.

#include <QMargins>
#include <QString>

#include <string_view>

class QHBoxLayout;
class QLabel;
class QWidget;

namespace wizz::app::grid_bar {

/// The bar's row layout: Padding (12, 8, 12, 8), no spacing (margins separate the controls).
[[nodiscard]] QHBoxLayout* make_row();
/// Adds `widget` with the outer `margin` (left, top, right, bottom), vertically centred.
void add(QHBoxLayout* row, QWidget* widget, QMargins margin, int stretch = 0);
/// The muted `_summary` label: plain text, no wrapping; a long text is clipped by the bar
/// instead of widening the window.
[[nodiscard]] QLabel* make_summary_label(QWidget* parent);
/// UTF-8 → QString.
[[nodiscard]] QString text(std::string_view utf8);

/// `_summary` margin and the label "Show:" margin.
inline constexpr QMargins kSummaryMargin{18, 0, 4, 0};
inline constexpr QMargins kLabelMargin{0, 0, 8, 0};
/// The view combo.
inline constexpr QMargins kDefaultMargin{0, 0, 0, 0};
/// The round-trip checkboxes.
inline constexpr QMargins kCheckBoxMargin{0, 0, 22, 0};

} // namespace wizz::app::grid_bar
