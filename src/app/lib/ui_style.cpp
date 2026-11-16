#include "ui_style.hpp"

#include "modern_style.hpp"

#include <QAccessibilityHints>
#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRawFont>
#include <QStyleFactory>
#include <QStyleHints>
#include <QToolTip>
#include <QWidget>

#include <cmath>
#include <map>
#include <tuple>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wizz::app::style {

namespace {

constexpr qreal kFormPointSize = 10.0;

/// The look in use and how it came about.
struct State {
    Theme theme = light_theme();
    std::optional<Appearance> appearance;  ///< none until apply_application_look()
    bool forced_scheme = false;            ///< setColorScheme() was called for WIZZ_COLOR_SCHEME
    bool following = false;                ///< connected to Windows' change signals
    bool refresh_pending = false;
    HighContrastProbe probe;
};

[[nodiscard]] State& state()
{
    static State instance;
    return instance;
}

[[nodiscard]] QFont with_families(const QStringList& list, qreal point_size)
{
    QFont font;
    font.setFamilies(list);
    font.setPointSizeF(point_size);
    return font;
}

/// `windows11` first unless WIZZ_FONTS=windows10, then the fallbacks.
[[nodiscard]] QStringList families(std::initializer_list<const char*> windows11, std::initializer_list<const char*> rest)
{
    QStringList list;
    if (!windows10_fonts()) {
        for (const char* family : windows11) {
            list.push_back(QString::fromLatin1(family));
        }
    }
    for (const char* family : rest) {
        list.push_back(QString::fromLatin1(family));
    }
    return list;
}

[[nodiscard]] bool system_high_contrast()
{
    HIGHCONTRASTW contrast{};
    contrast.cbSize = static_cast<UINT>(sizeof(contrast));
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, contrast.cbSize, &contrast, 0) != FALSE &&
           (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

[[nodiscard]] bool default_high_contrast()
{
    if (const std::optional<bool> forced = parse_switch(qgetenv("WIZZ_HIGH_CONTRAST"))) {
        return *forced;
    }
    return system_high_contrast();
}

[[nodiscard]] double channel(double value)
{
    return value <= 0.03928 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

[[nodiscard]] double luminance(const QColor& colour)
{
    return 0.2126 * channel(colour.redF()) + 0.7152 * channel(colour.greenF()) + 0.0722 * channel(colour.blueF());
}

/// Installs `look`: style, palette and tooltip palette.
void install(Appearance look)
{
    State& s = state();
    if (look == Appearance::high_contrast) {
        // The user's contrast theme wins: the plain native style draws the controls with the
        // system colours, and the palette is the system's (an empty palette resolves to the
        // platform theme's and keeps following it).
        QStyle* plain = QStyleFactory::create(QStringLiteral("windowsvista"));
        if (plain == nullptr) {
            plain = QStyleFactory::create(QStringLiteral("Windows"));
        }
        if (plain != nullptr) {
            QApplication::setStyle(plain);
            // setStyle() fills Qt's per-class fonts from Windows (Segoe UI 9 pt for item views,
            // menus, message boxes, tooltips); the app font replaces them, so every class uses it.
            QApplication::setFont(form_font());
        }
        QApplication::setPalette(QPalette());
        s.theme = high_contrast_theme(QApplication::palette());
        QToolTip::setPalette(QApplication::palette());
        return;
    }
    s.theme = look == Appearance::dark ? dark_theme() : light_theme();
    if (!ModernStyle::is_active()) {
        if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
            QApplication::setStyle(new ModernStyle(fusion));  // the application takes ownership
            QApplication::setFont(form_font());               // over the per-class fonts, see above
        }
    }
    const QPalette palette = make_palette(s.theme);
    QApplication::setPalette(palette);
    // Tooltips have a palette of their own, which Qt takes from Windows' (light) tooltip
    // colours; the panel is painted from the theme, so the text must follow it too.
    QToolTip::setPalette(palette);
}

/// The scheme Windows reports, without a WIZZ_COLOR_SCHEME request of ours in it.
[[nodiscard]] Qt::ColorScheme system_scheme(QStyleHints& hints, std::optional<Qt::ColorScheme> forced)
{
    State& s = state();
    if (forced) {
        if (!s.forced_scheme || hints.colorScheme() != *forced) {
            hints.setColorScheme(*forced);  // the title bar follows
            s.forced_scheme = true;
        }
        return *forced;
    }
    if (s.forced_scheme) {
        hints.unsetColorScheme();  // the override went away (tests): follow Windows again
        s.forced_scheme = false;
    }
    return hints.colorScheme();
}

void follow_windows()
{
    State& s = state();
    if (s.following) {
        return;
    }
    s.following = true;
    QStyleHints* hints = QGuiApplication::styleHints();
    QObject::connect(hints, &QStyleHints::colorSchemeChanged, theme_notifier(),
                     [] { schedule_appearance_refresh(); });
    QObject::connect(hints->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, theme_notifier(),
                     [] { schedule_appearance_refresh(); });
}

} // namespace

const Theme& theme()
{
    return state().theme;
}

Theme light_theme()
{
    Theme t;
    t.dark = false;
    t.window = QColor(0xF3, 0xF2, 0xF6);
    t.surface = QColor(0xFF, 0xFF, 0xFF);
    t.surface_alt = QColor(0xF8, 0xF7, 0xFA);
    t.input = QColor(0xFF, 0xFF, 0xFF);
    t.input_disabled = QColor(0xF4, 0xF3, 0xF6);
    t.border = QColor(0xE1, 0xDF, 0xE6);
    t.input_border = QColor(0xC6, 0xC2, 0xCD);
    t.input_border_hover = QColor(0x9A, 0x95, 0xA3);
    t.input_underline = QColor(0x8A, 0x85, 0x94);
    t.text = QColor(0x1C, 0x1B, 0x21);
    t.text_secondary = QColor(0x45, 0x42, 0x4D);
    t.text_muted = QColor(0x67, 0x63, 0x6F);
    t.text_disabled = QColor(0xA3, 0x9F, 0xAA);
    t.accent = QColor(0xC5, 0x00, 0x6E);
    t.accent_hover = QColor(0xB1, 0x00, 0x63);
    t.accent_pressed = QColor(0x96, 0x00, 0x54);
    t.accent_line = QColor(0xC5, 0x00, 0x6E);
    t.accent_text = QColor(0xC5, 0x00, 0x6E);
    t.on_accent = QColor(0xFF, 0xFF, 0xFF);
    t.selection = QColor(0xFB, 0xE2, 0xEF);
    t.selection_text = t.text;
    t.hover = QColor(0xF3, 0xF1, 0xF6);
    t.cheapest = QColor(0xE7, 0xF5, 0xEB);
    t.cheapest_selection = QColor(0xC6, 0xE9, 0xD0);
    t.success = QColor(0x1A, 0x7F, 0x37);
    t.failure = QColor(0xC4, 0x2B, 0x1C);
    t.warning = QColor(0x9A, 0x5B, 0x00);
    t.row_separator = QColor(0xEE, 0xED, 0xF1);
    t.track = QColor(0xE6, 0xE4, 0xEA);
    t.scroll_thumb = QColor(0xC9, 0xC5, 0xCF);
    t.scroll_thumb_hover = QColor(0x9E, 0x99, 0xA7);
    t.button = QColor(0xFF, 0xFF, 0xFF);
    t.button_hover = QColor(0xF4, 0xF2, 0xF7);
    t.button_pressed = QColor(0xEA, 0xE7, 0xEE);
    t.update_tint = QColor(0xFB, 0xEA, 0xF3);
    return t;
}

Theme dark_theme()
{
    Theme t;
    t.dark = true;
    t.window = QColor(0x13, 0x12, 0x17);
    t.surface = QColor(0x1D, 0x1C, 0x22);
    t.surface_alt = QColor(0x25, 0x24, 0x2B);
    t.input = QColor(0x27, 0x26, 0x2E);
    t.input_disabled = QColor(0x20, 0x1F, 0x25);
    t.border = QColor(0x2F, 0x2D, 0x36);
    t.input_border = QColor(0x47, 0x45, 0x50);
    t.input_border_hover = QColor(0x6C, 0x69, 0x76);
    t.input_underline = QColor(0x85, 0x81, 0x8F);
    t.text = QColor(0xEE, 0xEC, 0xF2);
    t.text_secondary = QColor(0xC9, 0xC6, 0xD1);
    t.text_muted = QColor(0xA3, 0x9F, 0xAC);
    t.text_disabled = QColor(0x6A, 0x67, 0x72);
    t.accent = QColor(0xC5, 0x00, 0x6E);
    t.accent_hover = QColor(0xD4, 0x1A, 0x80);
    t.accent_pressed = QColor(0xA8, 0x00, 0x5E);
    t.accent_line = QColor(0xF0, 0x5A, 0xA8);
    t.accent_text = QColor(0xF7, 0x72, 0xB8);
    t.on_accent = QColor(0xFF, 0xFF, 0xFF);
    t.selection = QColor(0x4B, 0x18, 0x34);
    t.selection_text = t.text;
    t.hover = QColor(0x33, 0x31, 0x3B);
    t.cheapest = QColor(0x17, 0x30, 0x22);
    t.cheapest_selection = QColor(0x22, 0x52, 0x36);
    t.success = QColor(0x5C, 0xCB, 0x85);
    t.failure = QColor(0xFF, 0x7A, 0x6E);
    t.warning = QColor(0xF5, 0xB0, 0x4A);
    t.row_separator = QColor(0x2A, 0x29, 0x31);
    t.track = QColor(0x34, 0x32, 0x3C);
    t.scroll_thumb = QColor(0x4D, 0x4A, 0x56);
    t.scroll_thumb_hover = QColor(0x6E, 0x6B, 0x79);
    t.button = QColor(0x2B, 0x2A, 0x32);
    t.button_hover = QColor(0x34, 0x33, 0x3C);
    t.button_pressed = QColor(0x24, 0x23, 0x2A);
    t.update_tint = QColor(0x36, 0x16, 0x29);
    return t;
}

Theme high_contrast_theme(const QPalette& system)
{
    const QColor window = system.color(QPalette::Active, QPalette::Window);
    const QColor window_text = system.color(QPalette::Active, QPalette::WindowText);
    const QColor base = system.color(QPalette::Active, QPalette::Base);
    const QColor highlight = system.color(QPalette::Active, QPalette::Highlight);
    const QColor highlighted_text = system.color(QPalette::Active, QPalette::HighlightedText);
    const QColor gray_text = system.color(QPalette::Disabled, QPalette::WindowText);
    const QColor button = system.color(QPalette::Active, QPalette::Button);

    Theme t;
    t.dark = window.lightness() < 128;
    t.high_contrast = true;
    t.window = window;
    t.surface = window;
    t.surface_alt = window;
    t.input = base;
    t.input_disabled = window;
    t.border = window_text;
    t.input_border = window_text;
    t.input_border_hover = highlight;
    t.input_underline = window_text;
    t.text = window_text;
    t.text_secondary = window_text;
    t.text_muted = window_text;
    t.text_disabled = gray_text;
    t.accent = highlight;
    t.accent_hover = highlight;
    t.accent_pressed = highlight;
    t.accent_line = highlight;
    t.accent_text = window_text;
    t.on_accent = highlighted_text;
    t.selection = highlight;
    t.selection_text = highlighted_text;
    t.hover = window;
    t.cheapest = window;
    t.cheapest_selection = highlight;
    t.success = window_text;
    t.failure = window_text;
    t.warning = window_text;
    t.row_separator = gray_text;
    t.track = gray_text;
    t.scroll_thumb = window_text;
    t.scroll_thumb_hover = highlight;
    t.button = button;
    t.button_hover = button;
    t.button_pressed = button;
    t.update_tint = window;
    return t;
}

QPalette make_palette(const Theme& t)
{
    QPalette p;
    const auto set_all = [&p](QPalette::ColorRole role, const QColor& colour) { p.setColor(role, colour); };
    set_all(QPalette::Window, t.window);
    set_all(QPalette::WindowText, t.text);
    set_all(QPalette::Base, t.input);
    set_all(QPalette::AlternateBase, t.surface_alt);
    set_all(QPalette::ToolTipBase, t.dark ? t.surface_alt : t.surface);
    set_all(QPalette::ToolTipText, t.text);
    set_all(QPalette::PlaceholderText, t.text_muted);
    set_all(QPalette::Text, t.text);
    set_all(QPalette::Button, t.button);
    set_all(QPalette::ButtonText, t.text);
    set_all(QPalette::BrightText, QColor(Qt::white));
    set_all(QPalette::Light, t.dark ? t.surface_alt : QColor(Qt::white));
    set_all(QPalette::Midlight, t.surface_alt);
    set_all(QPalette::Mid, t.border);
    set_all(QPalette::Dark, t.input_border);
    set_all(QPalette::Shadow, t.dark ? QColor(0, 0, 0) : QColor(0x8A, 0x86, 0x92));
    // Every selection is the soft tint with the normal text colour, like the grids': list
    // items, combo and completer popups, menus, the calendar, selected text.
    set_all(QPalette::Highlight, t.selection);
    set_all(QPalette::HighlightedText, t.selection_text);
    set_all(QPalette::Link, t.accent_text);
    set_all(QPalette::LinkVisited, t.accent_text);
    set_all(QPalette::Accent, t.accent);
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, t.text_disabled);
    }
    p.setColor(QPalette::Disabled, QPalette::Base, t.input_disabled);
    p.setColor(QPalette::Disabled, QPalette::Button, t.input_disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.input_disabled);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, t.text_disabled);
    return p;
}

double contrast_ratio(const QColor& a, const QColor& b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// --- Choosing the look ---------------------------------------------------------------------

std::optional<Qt::ColorScheme> parse_color_scheme(QByteArrayView value)
{
    const QByteArray text = value.toByteArray().trimmed().toLower();
    if (text == "dark") {
        return Qt::ColorScheme::Dark;
    }
    if (text == "light") {
        return Qt::ColorScheme::Light;
    }
    return std::nullopt;
}

std::optional<bool> parse_switch(QByteArrayView value)
{
    const QByteArray text = value.toByteArray().trimmed().toLower();
    if (text == "1" || text == "on" || text == "true" || text == "yes") {
        return true;
    }
    if (text == "0" || text == "off" || text == "false" || text == "no") {
        return false;
    }
    return std::nullopt;
}

Appearance choose_appearance(bool high_contrast, std::optional<Qt::ColorScheme> forced, Qt::ColorScheme system) noexcept
{
    if (high_contrast) {
        return Appearance::high_contrast;
    }
    return forced.value_or(system) == Qt::ColorScheme::Dark ? Appearance::dark : Appearance::light;
}

HighContrastProbe set_high_contrast_probe(HighContrastProbe probe)
{
    return std::exchange(state().probe, std::move(probe));
}

bool high_contrast_active()
{
    const HighContrastProbe& probe = state().probe;
    return probe ? probe() : default_high_contrast();
}

Appearance appearance()
{
    return state().appearance.value_or(Appearance::light);
}

ThemeNotifier* theme_notifier()
{
    static QPointer<ThemeNotifier> notifier;
    if (notifier.isNull()) {
        notifier = new ThemeNotifier(QCoreApplication::instance());
    }
    return notifier.data();
}

QColor tone_colour(TextTone tone)
{
    switch (tone) {
    case TextTone::secondary:
        return theme().text_secondary;
    case TextTone::muted:
        return theme().text_muted;
    case TextTone::normal:
        break;
    }
    return theme().text;
}

void set_text_tone(QWidget& widget, TextTone tone)
{
    const auto apply = [&widget, tone] {
        QPalette palette = widget.palette();
        palette.setColor(QPalette::WindowText, tone_colour(tone));
        widget.setPalette(palette);
    };
    apply();
    on_theme_change(&widget, apply);
}

bool apply_application_look()
{
    QApplication::setFont(form_font());
    const bool fusion = QStyleFactory::keys().contains(QStringLiteral("Fusion"), Qt::CaseInsensitive);
    state().appearance.reset();  // install whatever is chosen now
    refresh_appearance();
    follow_windows();
    return fusion;
}

bool refresh_appearance()
{
    State& s = state();
    s.refresh_pending = false;
    QStyleHints* hints = QGuiApplication::styleHints();
    const std::optional<Qt::ColorScheme> forced = parse_color_scheme(qgetenv("WIZZ_COLOR_SCHEME"));
    const bool contrast = high_contrast_active();
    const Qt::ColorScheme system = system_scheme(*hints, forced);
    const Appearance look = choose_appearance(contrast, forced, system);
    if (s.appearance == look) {
        bool same = true;
        if (look == Appearance::high_contrast) {
            // Windows may have switched to another contrast theme, whose colours the system
            // palette has now.
            const Theme now = high_contrast_theme(QApplication::palette());
            same = now.text == s.theme.text && now.window == s.theme.window && now.accent == s.theme.accent &&
                   now.input == s.theme.input;
        }
        if (same) {
            // Qt sets the tooltip palette back to Windows' on any theme change.
            QToolTip::setPalette(look == Appearance::high_contrast ? QApplication::palette() : make_palette(s.theme));
            return false;
        }
    }
    s.appearance = look;
    install(look);
    Q_EMIT theme_notifier()->changed();
    for (QWidget* widget : QApplication::allWidgets()) {
        widget->update();  // custom-painted widgets read the tokens when they paint
    }
    return true;
}

void schedule_appearance_refresh()
{
    State& s = state();
    if (s.refresh_pending) {
        return;
    }
    s.refresh_pending = true;
    // Queued: Qt finishes its own handling of the theme change (palettes, tooltip
    // palette) first, and the many ThemeChange events of one switch cost one refresh.
    QMetaObject::invokeMethod(theme_notifier(), [] { refresh_appearance(); }, Qt::QueuedConnection);
}

// --- Fonts and icons -----------------------------------------------------------------------

bool windows10_fonts()
{
    return qgetenv("WIZZ_FONTS").trimmed().toLower() == "windows10";
}

QFont form_font()
{
    return with_families(families({"Segoe UI Variable Text"}, {"Segoe UI"}), kFormPointSize);
}

QFont data_font()
{
    return with_families(families({"Segoe UI Variable Text"}, {"Segoe UI"}), 9.0);
}

QFont semibold_font(qreal point_size)
{
    // The semibold faces are families of their own in Windows' font list; asking for
    // weight 600 of "Segoe UI Variable Text" would give its Bold.
    return with_families(families({"Segoe UI Variable Text Semibold"}, {"Segoe UI Semibold"}), point_size);
}

QFont bold_form_font()
{
    return semibold_font(kFormPointSize);
}

QFont caption_font()
{
    QFont font = with_families(families({"Segoe UI Variable Small Semibold"}, {"Segoe UI Semibold"}), 8.0);
    font.setCapitalization(QFont::AllUppercase);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    return font;
}

QFont hint_font()
{
    return with_families(families({"Segoe UI Variable Text"}, {"Segoe UI"}), 9.0);
}

QFont mono_font(qreal point_size, bool semibold)
{
    // Windows 10 has Cascadia Mono only with Windows Terminal: Consolas then.
    QFont font = with_families(families({"Cascadia Mono"}, {"Consolas", "Courier New"}), point_size);
    font.setStyleHint(QFont::TypeWriter);
    font.setFixedPitch(true);
    if (semibold) {
        font.setWeight(QFont::DemiBold);
    }
    return font;
}

QFont icon_font(qreal point_size)
{
    // Segoe Fluent Icons ships with Windows 11; Windows 10 has the same code points in
    // Segoe MDL2 Assets.
    static const bool has_fluent = QFontDatabase::hasFamily(QStringLiteral("Segoe Fluent Icons"));
    QFont font(!windows10_fonts() && has_fluent ? QStringLiteral("Segoe Fluent Icons") : QStringLiteral("Segoe MDL2 Assets"));
    font.setPointSizeF(point_size);
    return font;
}

namespace {

/// The outline of `glyph` at the icon font's size, with its origin on the baseline;
/// computed once per glyph, size and family (QRawFont lookups are not cheap).
[[nodiscard]] const QPainterPath& icon_path(char16_t glyph, qreal point_size)
{
    static std::map<std::tuple<QString, char16_t, qint64>, QPainterPath> cache;
    const QFont font = icon_font(point_size);
    const auto key = std::make_tuple(font.family(), glyph, qRound64(point_size * 100));
    if (const auto found = cache.find(key); found != cache.end()) {
        return found->second;
    }
    QPainterPath path;
    const QRawFont raw = QRawFont::fromFont(font);
    const QList<quint32> indexes = raw.glyphIndexesForString(QString(QChar(glyph)));
    if (raw.isValid() && !indexes.isEmpty() && indexes.front() != 0) {
        path = raw.pathForGlyph(indexes.front());
    }
    return cache.emplace(key, std::move(path)).first->second;
}

} // namespace

void draw_icon(QPainter& painter, const QRectF& rect, char16_t glyph, qreal point_size, const QColor& colour,
               bool mirrored)
{
    QPainterPath path = icon_path(glyph, point_size);
    if (path.isEmpty()) {
        return;
    }
    const QRectF bounds = path.boundingRect();
    path.translate(-bounds.center());
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(rect.center());
    if (mirrored) {
        painter.scale(-1, 1);
    }
    painter.fillPath(path, colour);
    painter.restore();
}

QSizeF icon_size(char16_t glyph, qreal point_size)
{
    return icon_path(glyph, point_size).boundingRect().size();
}

QColor wizz_magenta()
{
    return QColor(197, 0, 110);
}

QColor gray_text()
{
    return theme().text_muted;
}

QColor search_button_disabled_text()
{
    return QColor(255, 255, 255, 178);
}

} // namespace wizz::app::style
