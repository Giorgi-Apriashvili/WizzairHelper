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
//   grids    Segoe UI Variable Text 9 pt   -> Segoe UI 9 pt (cells; headers semibold 10 pt)
//   semibold Segoe UI Variable Text Semibold -> Segoe UI Semibold
//   captions Segoe UI Variable Small Semibold 8 pt -> Segoe UI Semibold 8 pt
//   mono     Cascadia Mono (Windows Terminal on Windows 10) -> Consolas -> Courier New
//   icons    Segoe Fluent Icons            -> Segoe MDL2 Assets (same code points)
// All text is drawn "natural, slightly heavier" (D-58, TextRendering below): without hinting,
// the variable faces 50 units heavier on their weight axis.
//
// Diagnostic switches (environment, read when the look is evaluated; BUILDING.md):
//   WIZZ_COLOR_SCHEME=light|dark   forces a colour scheme;
//   WIZZ_HIGH_CONTRAST=1|0         says High Contrast is on / off instead of asking Windows;
//   WIZZ_FONTS=windows10           leaves out the Windows 11 families (Segoe UI Variable,
//                                  Cascadia, Segoe Fluent Icons) to check the fallbacks;
//   WIZZ_TEXT_RENDERING=hinted|natural  draws text as 2.0.0 did (hinted: its glyphs, and at 100 %
//                                  its layout) or without the extra weight (natural), for
//                                  comparisons and bug reports. Read when a font is built: set it
//                                  before the app starts.

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
    QColor selection;           ///< selected rows and items (grids, lists, menus, calendar days)
    QColor selection_text;
    QColor text_selection;      ///< selected text in inputs and the log, with on_accent text: the
                                ///< accent, lighter in dark (3:1 on the input, 4.5:1 for its text)
    QColor hover;               ///< a hovered list item, menu item or header section
    QColor cheapest;            ///< cheapest rows (01 §8.3)
    QColor cheapest_selection;  ///< cheapest row while selected
    QColor success;             ///< status line: done
    QColor failure;             ///< status line: failed
    QColor warning;             ///< wait note ("retrying in …")
    QColor row_separator;       ///< the line under every grid row
    QColor track;               ///< progress track
    QColor scroll_thumb;        ///< 3:1 on the surface
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
/// The QPalette of `theme` (Base = input background, Highlight = text_selection with
/// on_accent text, so selected text stands out in an input; ModernStyle gives list and
/// calendar items the selection tint of the grids instead).
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

/// How text is drawn (D-58). Qt 6.11 draws text through DirectWrite, and a font's hinting
/// preference picks the rendering mode: Qt's default hinting is DirectWrite GDI-classic at
/// 100 % (glyphs snapped to whole pixels, no vertical smoothing), QFont::PreferNoHinting is
/// natural symmetric (curves and diagonals smoothed vertically too, fractional positions).
/// Every font below gets the rendering, and so does every widget, popup and dialog through
/// the application font (QApplication::setFont clears Qt's per-class fonts); text stays
/// ClearType (Qt's DirectWrite engine ignores QFont::NoSubpixelAntialias).
enum class TextRendering {
    natural_heavier,  ///< "natural-heavier", the default: natural, and every face with a weight
                      ///< axis (Segoe UI Variable, Cascadia Mono) kHeavierBy units heavier than the
                      ///< face it draws, for the ink unhinted 9-10 pt strokes lose. Faces without
                      ///< one (the Windows 10 fallbacks Segoe UI, Consolas) are plain natural.
    natural,          ///< "natural": natural, every face at its own weight
    hinted,           ///< "hinted": Qt's default hinting, the faces' own weights (2.0.0)
};
/// The units natural-heavier adds to a face's weight: regular 400 -> 450, semibold 600 -> 650
/// (a bold 700 stays at the axis' maximum, 700, for Segoe UI Variable).
inline constexpr int kHeavierBy = 50;
/// A WIZZ_TEXT_RENDERING value (trimmed, any case): "natural-heavier", "natural" or "hinted";
/// anything else is none.
[[nodiscard]] std::optional<TextRendering> parse_text_rendering(QByteArrayView value);
/// The rendering WIZZ_TEXT_RENDERING asks for; natural-heavier when it is unset or unknown.
[[nodiscard]] TextRendering text_rendering();
/// Its WIZZ_TEXT_RENDERING name.
[[nodiscard]] const char* text_rendering_name(TextRendering rendering) noexcept;

/// The weight-axis ("wght") value `font` draws with under `rendering`: for natural-heavier and a
/// face with a weight axis, the weight of the face Qt picks for the font's families and weight
/// (QFontInfo, without any axis) plus kHeavierBy, within the axis' range; else none. So a
/// regular face gets 450, a semibold family 650 whatever weight the font asks for (it has no
/// other face), and a bold copy of a regular font (setBold(), weight 700) gets 700.
[[nodiscard]] std::optional<float> weight_axis_for(const QFont& font, TextRendering rendering);
/// Gives `font` the hinting preference of the rendering WIZZ_TEXT_RENDERING asks for and the
/// weight axis of weight_axis_for() (or none); returns whether a value changed. A model's
/// Qt::FontRole, QPainter::setFont and a widget's own font are resolved against another font, and
/// QFont::resolve() replaces what a font only copied: so the hinting becomes the font's own, and so
/// does the axis of a font that names its own families, also where the values were already right.
/// A font that takes its families from the font it is resolved against takes that font's axis
/// along, which is the right one for those families when that font is fitted. The fonts below come
/// out of it; a copy of one with another weight or family (Qt's setBold() copies) needs it again.
bool fit_text_rendering(QFont& font);
/// fit_text_rendering() for `widget`'s font: sets it when it changed, which makes every
/// attribute the widget inherited its own; returns whether it did.
bool fit_text_rendering(QWidget& widget);
/// Keeps every widget's font fitted: an application event filter that runs
/// fit_text_rendering(widget) on each QEvent::Polish and QEvent::FontChange, in every look
/// (High Contrast's plain style included). That covers the fonts Qt derives from a widget
/// font (the calendar's bold month and year buttons) and fonts the app builds without the
/// functions below (a default-constructed QFont copies the application font, axis included).
/// A widget whose parent's font needs fitting too is left alone: Qt sends the parent's
/// FontChange after its children's, and the parent's fitted font reaches them, so a child
/// never holds a weight it only inherited. apply_application_look() installs it; later calls
/// do nothing.
void keep_text_rendering();
// Each font below names its face: its families, and the regular ones their weight (QFont::Normal),
// so its hinting and weight axis are its own too (fit_text_rendering): resolved against a bolder
// font, it still draws as it was built. The semibold families leave the weight open: they have one
// face.

/// The UI font: Segoe UI Variable Text 10 pt, Segoe UI 10 pt on Windows 10.
[[nodiscard]] QFont form_font();
/// The result grids' cells: Segoe UI Variable Text 9 pt (Segoe UI 9 pt), denser than the
/// UI font so more rows fit; their headers stay at the UI size, semibold.
[[nodiscard]] QFont data_font();
/// Its semibold version (search buttons, table headers).
[[nodiscard]] QFont bold_form_font();
/// Segoe UI Variable Text Semibold (Segoe UI Semibold on Windows 10) at `point_size`.
[[nodiscard]] QFont semibold_font(qreal point_size);
/// The same families without a size: a font that takes the size of the font it is resolved
/// against (bold grid cells, the busy panel's wait note). A weight of 600 on "Segoe UI
/// Variable Text" would give its Bold.
[[nodiscard]] QFont semibold_face();
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

} // namespace wizz::app::style
