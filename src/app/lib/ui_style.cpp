#include "ui_style.hpp"

#include "modern_style.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QStyleFactory>
#include <QStyleHints>

namespace wizz::app::style {

namespace {

[[nodiscard]] Theme& current()
{
    static Theme theme = light_theme();
    return theme;
}

[[nodiscard]] QFont with_families(std::initializer_list<QString> families, qreal point_size)
{
    QFont font;
    font.setFamilies(QStringList(families));
    font.setPointSizeF(point_size);
    return font;
}

/// The scheme WIZZ_COLOR_SCHEME forces, if any.
[[nodiscard]] std::optional<Qt::ColorScheme> forced_scheme()
{
    const QByteArray value = qgetenv("WIZZ_COLOR_SCHEME").trimmed().toLower();
    if (value == "dark") {
        return Qt::ColorScheme::Dark;
    }
    if (value == "light") {
        return Qt::ColorScheme::Light;
    }
    return std::nullopt;
}

void use_scheme(Qt::ColorScheme scheme)
{
    current() = scheme == Qt::ColorScheme::Dark ? dark_theme() : light_theme();
    QApplication::setPalette(make_palette(current()));
}

} // namespace

const Theme& theme()
{
    return current();
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
    t.on_accent = QColor(0xFF, 0xFF, 0xFF);
    t.selection = QColor(0xFB, 0xE2, 0xEF);
    t.selection_text = t.text;
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
    t.on_accent = QColor(0xFF, 0xFF, 0xFF);
    t.selection = QColor(0x4B, 0x18, 0x34);
    t.selection_text = t.text;
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
    set_all(QPalette::Highlight, t.accent);
    set_all(QPalette::HighlightedText, t.on_accent);
    set_all(QPalette::Link, t.accent_line);
    set_all(QPalette::LinkVisited, t.accent_line);
    set_all(QPalette::Accent, t.accent);
    for (const QPalette::ColorRole role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        p.setColor(QPalette::Disabled, role, t.text_disabled);
    }
    p.setColor(QPalette::Disabled, QPalette::Base, t.input_disabled);
    p.setColor(QPalette::Disabled, QPalette::Button, t.input_disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.text_disabled);
    return p;
}

QFont form_font()
{
    return with_families({QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI")}, 10.0);
}

QFont semibold_font(qreal point_size)
{
    // The semibold faces are families of their own in Windows' font list; asking for
    // weight 600 of "Segoe UI Variable Text" would give its Bold.
    return with_families({QStringLiteral("Segoe UI Variable Text Semibold"), QStringLiteral("Segoe UI Semibold")},
                         point_size);
}

QFont bold_form_font()
{
    return semibold_font(10.0);
}

QFont caption_font()
{
    QFont font = with_families(
        {QStringLiteral("Segoe UI Variable Small Semibold"), QStringLiteral("Segoe UI Semibold")}, 8.0);
    font.setCapitalization(QFont::AllUppercase);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    return font;
}

QFont hint_font()
{
    return with_families({QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI")}, 9.0);
}

QFont mono_font(qreal point_size, bool semibold)
{
    QFont font = with_families(
        {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"), QStringLiteral("Courier New")}, point_size);
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
    static const QString family = QFontDatabase::hasFamily(QStringLiteral("Segoe Fluent Icons"))
                                      ? QStringLiteral("Segoe Fluent Icons")
                                      : QStringLiteral("Segoe MDL2 Assets");
    QFont font(family);
    font.setPointSizeF(point_size);
    return font;
}

namespace {

/// The outline of `glyph` at the icon font's size, with its origin on the baseline.
[[nodiscard]] QPainterPath icon_path(char16_t glyph, qreal point_size)
{
    const QRawFont raw = QRawFont::fromFont(icon_font(point_size));
    const QList<quint32> indexes = raw.glyphIndexesForString(QString(QChar(glyph)));
    if (!raw.isValid() || indexes.isEmpty() || indexes.front() == 0) {
        return {};
    }
    return raw.pathForGlyph(indexes.front());
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

bool apply_application_look()
{
    QStyleHints* hints = QGuiApplication::styleHints();
    const std::optional<Qt::ColorScheme> forced = forced_scheme();
    if (forced) {
        hints->setColorScheme(*forced);  // the title bar follows
    }
    const Qt::ColorScheme scheme = forced.value_or(hints->colorScheme());
    current() = scheme == Qt::ColorScheme::Dark ? dark_theme() : light_theme();

    QApplication::setFont(form_font());
    QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"));
    if (fusion == nullptr) {
        return false;
    }
    QApplication::setStyle(new ModernStyle(fusion));  // the application takes ownership
    QApplication::setPalette(make_palette(current()));
    if (!forced) {
        QObject::connect(hints, &QStyleHints::colorSchemeChanged, qApp, [](Qt::ColorScheme changed) {
            use_scheme(changed);
        });
    }
    return true;
}

QColor search_button_disabled_text()
{
    return QColor(255, 255, 255, 178);
}

std::optional<QSize> prototype_window_size()
{
    const QByteArray value = qgetenv("WIZZ_PROTO_WINDOW").trimmed().toLower();
    const qsizetype x = value.indexOf('x');
    if (x <= 0) {
        return std::nullopt;
    }
    bool width_ok = false;
    bool height_ok = false;
    const int width = value.left(x).toInt(&width_ok);
    const int height = value.mid(x + 1).toInt(&height_ok);
    if (!width_ok || !height_ok || width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return QSize(width, height);
}

} // namespace wizz::app::style
