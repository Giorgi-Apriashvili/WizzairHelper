#pragma once
// Fonts and colours the desktop spec names (01 §4.3-§4.5, §10.8). Sizes are
// Qt logical pixels, which already are 01's L(x) (GUI_DESIGN.md).

#include <QColor>
#include <QFont>

namespace wizz::app::style {

/// The WinForms default font, Segoe UI 9 pt (01 §4.5 "Fonts").
[[nodiscard]] QFont form_font();
/// Its bold version (search buttons, 01 §4.5).
[[nodiscard]] QFont bold_form_font();

/// Wizz magenta RGB(197, 0, 110) = #C5006E: search buttons and the spinner arc.
[[nodiscard]] QColor wizz_magenta();
/// SystemColors.GrayText (hints, `_resolved`, grid summaries).
[[nodiscard]] QColor gray_text();

/// The look the 01 metrics assume; call once after the QApplication exists. It
/// selects the native-control "windowsvista" style (WinForms draws comctl32
/// controls; the Windows 11 style's side-by-side spin buttons need about 80 px and
/// hide the value of the L(48)-L(64) numeric boxes) and the form font. Returns
/// whether the style was available.
bool apply_application_look();

/// Search-button text colour while disabled (01 §4.5 OPEN QUESTION: "GrayText or a
/// lighter tint"): white blended 60 % over the magenta, which stays readable.
[[nodiscard]] QColor search_button_disabled_text();

} // namespace wizz::app::style
