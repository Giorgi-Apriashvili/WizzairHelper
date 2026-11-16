#pragma once
// The application look (DECISIONS D-53): Fusion under the ModernStyle proxy
// (modern_style.hpp), one light and one dark set of colour tokens (Theme), and the
// fonts below. Every widget takes its colours from theme() and takes them again
// when the look changes (theme_notifier()), so a Windows switch between light and
// dark applies at once, without a restart.
//
// Which look shows (choose_appearance):
//   1. Windows High Contrast (SPI_GETHIGHCONTRAST) wins: the plain native style
//      (windowsvista), the system palette, and tokens taken from that palette, so the
//      user's contrast theme is what the app shows (D-55);
//   2. else WIZZ_COLOR_SCHEME=light|dark when it is set: a test and diagnostic
//      override, like QT_SCALE_FACTOR (the title bar follows);
//   3. else the Windows app mode (QStyleHints::colorScheme), followed while the app
//      runs.
// QStyleHints::colorSchemeChanged, QAccessibilityHints::contrastPreferenceChanged and
// the main window's QEvent::ThemeChange schedule a re-evaluation (refresh_appearance).
//
// Fonts, with their Windows 10 fallbacks (QFont::setFamilies picks the first
// installed family):
//   UI       Segoe UI Variable Text 10 pt  -> Segoe UI 10 pt
//   semibold Segoe UI Variable Text Semibold -> Segoe UI Semibold
//   captions Segoe UI Variable Small Semibold 8 pt -> Segoe UI Semibold 8 pt
//   mono     Cascadia Mono (Windows Terminal on Windows 10) -> Consolas -> Courier New
//   icons    Segoe Fluent Icons            -> Segoe MDL2 Assets (same code points)
//
// Diagnostic switches (environment, read when the look is evaluated; BUILDING.md):
//   WIZZ_COLOR_SCHEME=light|dark   forces a colour scheme;
//   WIZZ_HIGH_CONTRAST=1|0         says High Contrast is on / off instead of asking Windows;
//   WIZZ_FONTS=windows10           leaves out the Windows 11 families (Segoe UI Variable,
//                                  Cascadia, Segoe Fluent Icons) to check the fallbacks.

#include <QByteArrayView>
#include <QColor>
#include <QFont>
#include <QObject>
#include <QPalette>
#include <QString>

#include <functional>
#include <optional>

class QPainter;
class QRectF;
class QSizeF;
class QWidget;

namespace wizz::app::style {

/// What the app shows.
enum class Appearance { light, dark, high_contrast };

/// The colour tokens of one look. Contrast of text, text_secondary and text_muted on
/// window, surface, surface_alt, input, selection and cheapest is at least 4.5:1
/// (look_test.cpp checks every pair).
struct Theme {
    bool dark = false;
    bool high_contrast = false;  ///< tokens from the system palette (High Contrast)
    QColor window;              ///< window background behind the cards
    QColor surface;             ///< cards, tables, the log, the status bar, popups
    QColor surface_alt;         ///< table header, calendar header row
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
    QColor accent_line;         ///< focus rings, the selected tab, icons (3:1 on every surface)
    QColor accent_text;         ///< accent-coloured text (calendar weekends; 4.5:1 on input and selection)
    QColor on_accent;           ///< text on accent fills
    QColor selection;           ///< selected rows and items (grids, lists, menus, text)
    QColor selection_text;
    QColor hover;               ///< a hovered list item, menu item or header section
    QColor cheapest;            ///< cheapest rows (01 §8.3)
    QColor cheapest_selection;  ///< cheapest row while selected
    QColor success;             ///< status line: done
    QColor failure;             ///< status line: failed
    QColor warning;             ///< wait note ("retrying in …")
    QColor row_separator;       ///< the line under every grid row
    QColor track;               ///< progress track
    QColor scroll_thumb;
    QColor scroll_thumb_hover;
    QColor button;              ///< secondary button fill
    QColor button_hover;
    QColor button_pressed;
    QColor update_tint;         ///< update bar background
};

/// The current look (light until apply_application_look() chose one).
[[nodiscard]] const Theme& theme();
[[nodiscard]] Theme light_theme();
[[nodiscard]] Theme dark_theme();
/// High Contrast: every token is a colour of the system palette `system` (Window,
/// WindowText, Base, Text, Highlight, HighlightedText, Button, GrayText); nothing is
/// mixed or tinted, and the status colours are the text colour.
[[nodiscard]] Theme high_contrast_theme(const QPalette& system);
/// The QPalette of `theme` (Base = input background, Highlight = the selection tint, so
/// list, menu, calendar and text selections all look like the grids').
[[nodiscard]] QPalette make_palette(const Theme& theme);
/// WCAG 2 contrast ratio of two opaque colours (1..21).
[[nodiscard]] double contrast_ratio(const QColor& a, const QColor& b);

// --- Choosing the look -----------------------------------------------------------------

/// A WIZZ_COLOR_SCHEME value: "light" or "dark" (trimmed, any case); anything else is none.
[[nodiscard]] std::optional<Qt::ColorScheme> parse_color_scheme(QByteArrayView value);
/// An on/off switch value: 1/on/true/yes or 0/off/false/no (trimmed, any case); else none.
[[nodiscard]] std::optional<bool> parse_switch(QByteArrayView value);
/// High Contrast wins, then the forced scheme, then the system's (Unknown = light).
[[nodiscard]] Appearance choose_appearance(bool high_contrast, std::optional<Qt::ColorScheme> forced,
                                           Qt::ColorScheme system) noexcept;

/// Tells whether Windows High Contrast is on. The default asks WIZZ_HIGH_CONTRAST, then
/// SystemParametersInfoW(SPI_GETHIGHCONTRAST); tests inject their own.
using HighContrastProbe = std::function<bool()>;
/// Replaces the probe (empty = the default) and returns the previous one.
HighContrastProbe set_high_contrast_probe(HighContrastProbe probe);
[[nodiscard]] bool high_contrast_active();

/// The look in use.
[[nodiscard]] Appearance appearance();

/// Emits changed() after a new look was installed (theme, style, palette, tooltip
/// palette). Widgets that keep token colours in their own palettes, models that cache
/// brushes, and anything else that copies a token connect to it.
class ThemeNotifier final : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

Q_SIGNALS:
    void changed();
};
/// The application's notifier (owned by the QApplication).
[[nodiscard]] ThemeNotifier* theme_notifier();
/// Runs `apply` after every change of the look while `context` lives (not now).
template <class Apply>
QMetaObject::Connection on_theme_change(const QObject* context, Apply&& apply)
{
    return QObject::connect(theme_notifier(), &ThemeNotifier::changed, context, std::forward<Apply>(apply));
}

/// The colour a label's text takes from the theme.
enum class TextTone { normal, secondary, muted };
[[nodiscard]] QColor tone_colour(TextTone tone);
/// Gives `widget`'s WindowText the colour of `tone`, now and after every change of the look.
void set_text_tone(QWidget& widget, TextTone tone);

/// Installs the look: the UI font, then the style, palette and tooltip palette of
/// choose_appearance(), and starts following Windows' changes. Call once after the
/// QApplication exists. Returns whether Fusion was available.
bool apply_application_look();
/// Evaluates the look again (Windows' app mode and contrast, the environment) and installs
/// it when it differs from the one in use; then theme_notifier() emits and every widget
/// repaints. Returns whether the look changed. Also what a test calls after changing
/// WIZZ_COLOR_SCHEME or the high-contrast probe.
bool refresh_appearance();
/// refresh_appearance() from the event loop, once for any number of calls before it runs
/// (a theme change reaches every window).
void schedule_appearance_refresh();

// --- Fonts and icons --------------------------------------------------------------------

/// WIZZ_FONTS=windows10: the Windows 11 font families are left out.
[[nodiscard]] bool windows10_fonts();
/// The UI font: Segoe UI Variable Text 10 pt, Segoe UI 10 pt on Windows 10.
[[nodiscard]] QFont form_font();
/// Its semibold version (search buttons, table headers).
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
/// private-use code points. Outlines are cached per glyph, size and font.
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

/// Wizz magenta RGB(197, 0, 110) = #C5006E: the accent of the light and dark looks.
[[nodiscard]] QColor wizz_magenta();
/// Secondary text (hints, `_resolved`, grid summaries): theme().text_muted.
[[nodiscard]] QColor gray_text();
/// Search-button text colour while disabled: white at 70 % over the faded magenta.
[[nodiscard]] QColor search_button_disabled_text();

} // namespace wizz::app::style
