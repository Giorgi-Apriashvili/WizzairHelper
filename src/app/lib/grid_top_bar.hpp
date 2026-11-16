#pragma once
// The top bar of both result grids (01 §8.1, §9.1), in the look of DECISIONS
// D-53: one row at the top of the results card, padding (12, 4, 12, 4), its
// controls vertically centred and separated by their own margins; the summary in
// the theme's muted text colour.

#include <QMargins>
#include <QString>

#include <string_view>

class QHBoxLayout;
class QLabel;
class QWidget;

namespace wizz::app::grid_bar {

/// Padding of the bar.
inline constexpr QMargins kPadding{12, 4, 12, 4};
/// The bar's row layout: kPadding, no spacing (margins separate the controls).
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
