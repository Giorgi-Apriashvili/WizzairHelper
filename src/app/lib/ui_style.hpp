#pragma once
// The application look (prototype "layout" direction): Fusion with the
// ModernStyle proxy (modern_style.hpp), one light and one dark palette chosen
// from the Windows colour scheme, and the fonts below. Every widget takes its
// colours from theme() so light and dark stay consistent.
//
// Fonts, with their Windows 10 fallbacks (QFont::setFamilies picks the first
// installed family):
//   UI       Segoe UI Variable Text 10 pt  -> Segoe UI 10 pt
//   captions Segoe UI Variable Small       -> Segoe UI
//   mono     Cascadia Mono                 -> Consolas -> Courier New
//   icons    Segoe Fluent Icons            -> Segoe MDL2 Assets (same code points)
//
// Prototype-only switches (environment):
//   WIZZ_COLOR_SCHEME=light|dark  forces a scheme (the title bar follows);
//   WIZZ_PROTO_WINDOW=980x600     start size of the window in logical pixels.

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QSize>
#include <QString>

#include <optional>

class QPainter;

namespace wizz::app::style {

/// The colour tokens of one scheme. Contrast of text on `window`, `surface` and
/// `surface_alt` is at least 4.5:1 for text, text_secondary and text_muted.
struct Theme {
    bool dark = false;
    QColor window;              ///< window background behind the cards
    QColor surface;             ///< cards, tables, the log, the status bar
    QColor surface_alt;         ///< table header, hover fills
    QColor input;               ///< input field background
    QColor input_disabled;      ///< input field background while disabled
    QColor border;              ///< card borders and dividers
    QColor input_border;        ///< input frames
    QColor input_border_hover;  ///< input frames under the mouse
    QColor input_underline;     ///< the darker bottom stroke of inputs (3:1 on the input)
    QColor text;                ///< primary text
    QColor text_secondary;      ///< field labels, header labels
    QColor text_muted;          ///< hints, captions, summaries ("GrayText")
    QColor text_disabled;       ///< disabled text
    QColor accent;              ///< the app's magenta #C5006E: primary buttons, progress
    QColor accent_hover;
    QColor accent_pressed;
    QColor accent_line;         ///< focus rings, the selected tab, accent text (3:1 on window)
    QColor on_accent;           ///< text on accent fills
    QColor selection;           ///< selected grid row
    QColor selection_text;
    QColor cheapest;            ///< cheapest rows (01 §8.3)
    QColor cheapest_selection;  ///< cheapest row while selected
    QColor success;             ///< status line: done
    QColor failure;             ///< status line: failed
    QColor warning;             ///< wait note ("retrying in …")
    QColor row_separator;       ///< the line under every grid row
    QColor track;               ///< progress track, checkbox off
    QColor scroll_thumb;
    QColor scroll_thumb_hover;
    QColor button;              ///< secondary button fill
    QColor button_hover;
    QColor button_pressed;
    QColor update_tint;         ///< update bar background
};

/// The current scheme (light until apply_application_look() chose one).
[[nodiscard]] const Theme& theme();
[[nodiscard]] Theme light_theme();
[[nodiscard]] Theme dark_theme();
/// The QPalette of `theme` (Base = input background, Highlight = accent).
[[nodiscard]] QPalette make_palette(const Theme& theme);

/// The UI font: Segoe UI Variable Text 10 pt, Segoe UI 10 pt on Windows 10.
[[nodiscard]] QFont form_font();
/// Its semibold version (search buttons, the busy panel's emphasis).
[[nodiscard]] QFont bold_form_font();
/// Segoe UI Variable Text Semibold (Segoe UI Semibold on Windows 10) at `point_size`.
[[nodiscard]] QFont semibold_font(qreal point_size);
/// Section captions of the input cards: small, semibold, upper case, tracked.
[[nodiscard]] QFont caption_font();
/// Hints next to the captions and other secondary text: 9 pt.
[[nodiscard]] QFont hint_font();
/// The monospace font (log, elapsed time): Cascadia Mono, else Consolas.
[[nodiscard]] QFont mono_font(qreal point_size, bool semibold = false);
/// Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets (Windows 10).
[[nodiscard]] QFont icon_font(qreal point_size);

/// Paints icon `glyph` of icon_font(point_size) centred in `rect`. The glyph's outline
/// is filled directly (QRawFont): Qt's shaped text drawing renders nothing for these
/// private-use code points.
void draw_icon(QPainter& painter, const QRectF& rect, char16_t glyph, qreal point_size, const QColor& colour,
               bool mirrored = false);
/// The ink size of `glyph` at `point_size` (logical pixels).
[[nodiscard]] QSizeF icon_size(char16_t glyph, qreal point_size);

/// Icon glyphs (the same code points in both icon fonts).
namespace glyph {
inline constexpr char16_t kSearch = 0xE721;
inline constexpr char16_t kMapPin = 0xE707;
inline constexpr char16_t kAirplane = 0xE709;
inline constexpr char16_t kPeople = 0xE716;
inline constexpr char16_t kInfo = 0xE946;
} // namespace glyph

/// Wizz magenta RGB(197, 0, 110) = #C5006E: primary buttons and the spinner arc.
[[nodiscard]] QColor wizz_magenta();
/// Secondary text (hints, `_resolved`, grid summaries): theme().text_muted.
[[nodiscard]] QColor gray_text();

/// Fusion + ModernStyle, the palette of the Windows colour scheme (or of
/// WIZZ_COLOR_SCHEME) and the UI font; follows later scheme changes. Call once
/// after the QApplication exists. Returns whether Fusion was available.
bool apply_application_look();

/// Search-button text colour while disabled: white at 70 % over the faded magenta.
[[nodiscard]] QColor search_button_disabled_text();

/// The WIZZ_PROTO_WINDOW start size, when set ("980x600").
[[nodiscard]] std::optional<QSize> prototype_window_size();

} // namespace wizz::app::style
