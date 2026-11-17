#include "settings_button.hpp"

#include "modern_style.hpp"
#include "qt_strings.hpp"
#include "ui_style.hpp"

#include "wizz/ui/update_model.hpp"

#include <QFontMetricsF>
#include <QPainter>
#include <QStyleOptionButton>
#include <QStyleOptionFocusRect>
#include <QTabBar>
#include <QTabWidget>
#include <QtMath>

#include <algorithm>

namespace wizz::app {

SettingsButton::SettingsButton(QWidget* parent) : QPushButton(parent)
{
    const QString title = to_qstring(ui::update_strings::kSettingsTitle);
    setText(title);
    setAccessibleName(title);
    setToolTip(title);
    setObjectName(QStringLiteral("settingsButton"));
    setFlat(true);
    setAutoDefault(false);
    setDefault(false);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

int SettingsButton::glyph_width() const
{
    return qCeil(style::icon_size(style::glyph::kSettings, kGlyphPointSize).width());
}

QSize SettingsButton::sizeHint() const
{
    ensurePolished();
    const int text_width = qCeil(QFontMetricsF(font()).horizontalAdvance(text()));
    const int width = kPadding.left() + glyph_width() + kGlyphGap + text_width + kPadding.right();
    int height = ModernStyle::control_height(fontMetrics());
    if (const auto* tabs = qobject_cast<const QTabWidget*>(parentWidget()); tabs != nullptr && tabs->tabBar() != nullptr) {
        // QTabWidget gives a corner widget at most the tab bar's height less the style's base.
        height = tabs->tabBar()->sizeHint().height() - tabs->style()->pixelMetric(QStyle::PM_TabBarBaseHeight, nullptr, tabs);
    }
    return QSize{width, std::max(height, 1)};
}

QColor SettingsButton::glyph_colour() const
{
    const style::Theme& theme = style::theme();
    return isEnabled() ? theme.accent_line : theme.text_disabled;
}

QColor SettingsButton::text_colour() const
{
    const style::Theme& theme = style::theme();
    if (!isEnabled()) {
        return theme.text_disabled;
    }
    return underMouse() || isDown() ? theme.text : theme.text_secondary;  // like a tab's label
}

void SettingsButton::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    QStyleOptionButton option;
    initStyleOption(&option);

    // The face: the style's flat button (a fill under the mouse and while pressed), inset like
    // a tab's hover fill so the two read as one row.
    QStyleOptionButton face = option;
    face.rect = rect().marginsRemoved(kFaceMargins);
    style()->drawControl(QStyle::CE_PushButtonBevel, &face, &painter, this);

    // The gear and the text, left to right inside the padding, centred down the button.
    const QRect content = rect().marginsRemoved(kPadding);
    const int glyph = glyph_width();
    style::draw_icon(painter, QRectF(content.left(), 0, glyph, height()), style::glyph::kSettings, kGlyphPointSize,
                     glyph_colour());
    painter.setFont(font());
    painter.setPen(text_colour());
    const int text_left = content.left() + glyph + kGlyphGap;
    painter.drawText(QRect(text_left, 0, std::max(0, width() - text_left), height()),
                     Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, text());

    // Keyboard focus: the style's ring (the accent, like the tabs'; a dotted frame in High
    // Contrast), only after a keyboard focus change.
    if (option.state.testFlag(QStyle::State_HasFocus) && option.state.testFlag(QStyle::State_KeyboardFocusChange)) {
        QStyleOptionFocusRect focus;
        focus.QStyleOption::operator=(option);
        focus.rect = rect().marginsRemoved(kFocusMargins);
        style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
    }
}

} // namespace wizz::app
