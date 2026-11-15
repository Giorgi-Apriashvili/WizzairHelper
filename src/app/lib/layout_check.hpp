#pragma once
// Automated part of the 01 §15 / 05 §5 DPI inspection: "no clipping,
// overlapping controls or hidden Cancel button is acceptable".
//
// layout_problems() walks every visible layout under a window and reports, for
// each visible widget a layout places:
//   * "clipped by its parent"  — its rectangle leaves the parent's rectangle;
//   * "cut off by the window"  — some ancestor clips part of it (it is not fully
//                                visible inside the window's client area);
//   * "smaller than its text"  — a label or button narrower / lower than its
//                                minimum size hint, so its text is cut (labels that
//                                elide or wrap by design, and widgets with an
//                                Ignored size policy, are exempt);
//   * "overlaps …"             — two visible widgets of the same layout intersect;
// and for a QTabBar whose tabs do not fit ("tabs do not fit").
//
// Each problem is one line "<widget>: <problem>", where <widget> is the class name
// plus the object name or visible text. Test-only diagnostics of --autotest (D-30)
// and the widget tests; UI thread only.

#include <string>
#include <vector>

class QWidget;

namespace wizz::app {

/// The problems found under `root` (root included), in tree order; empty when none.
[[nodiscard]] std::vector<std::string> layout_problems(const QWidget& root);

/// `widget` is visible and lies completely inside its window's client area: no
/// ancestor clips any part of it.
[[nodiscard]] bool is_fully_visible(const QWidget& widget);

/// "<class>" plus " '<object name or text>'" when there is one (problem reports).
[[nodiscard]] std::string describe_widget(const QWidget& widget);

} // namespace wizz::app
