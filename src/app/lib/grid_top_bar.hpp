#pragma once
// The top bar of both result grids (01 §8.1, §9.1): a WinForms FlowLayoutPanel
// docked at the top, AutoSize, no wrapping, Padding (0, 2, 0, 2), whose
// controls keep their own outer Margin and are top-aligned.

#include <QMargins>
#include <QString>

#include <string_view>

class QHBoxLayout;
class QLabel;
class QWidget;

namespace wizz::app::grid_bar {

/// The bar's row layout: Padding (0, 2, 0, 2), no spacing (margins separate the controls).
[[nodiscard]] QHBoxLayout* make_row();
/// Adds `widget` with the WinForms outer `margin` (left, top, right, bottom), top-aligned.
void add(QHBoxLayout* row, QWidget* widget, QMargins margin, int stretch = 0);
/// The grey `_summary` label: plain text, no wrapping; a long text is clipped by the bar
/// instead of widening the window.
[[nodiscard]] QLabel* make_summary_label(QWidget* parent);
/// UTF-8 → QString.
[[nodiscard]] QString text(std::string_view utf8);

/// `_summary` Margin (14, 7, 3, 0) and the label "Show:" Margin (3, 7, 3, 0).
inline constexpr QMargins kSummaryMargin{14, 7, 3, 0};
inline constexpr QMargins kLabelMargin{3, 7, 3, 0};
/// WinForms default control Margin (3, 3, 3, 3) (the view combo).
inline constexpr QMargins kDefaultMargin{3, 3, 3, 3};
/// The round-trip checkboxes' Margin (3, 5, 12, 0).
inline constexpr QMargins kCheckBoxMargin{3, 5, 12, 0};

} // namespace wizz::app::grid_bar
