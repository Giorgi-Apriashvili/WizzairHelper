#include "grid_bar_button.hpp"

#include "ui_style.hpp"

#include <QFontMetricsF>
#include <QPainter>
#include <QStyleOptionButton>
#include <QStyleOptionFocusRect>
#include <QtMath>

#include <algorithm>

namespace wizz::app {

GridBarButton::GridBarButton(char16_t glyph, const QString& text, const QString& tool_tip, QWidget* parent)
    : QPushButton(parent), glyph_(glyph)
{
    setText(text);
    setAccessibleName(text);
    setToolTip(tool_tip);
    setFlat(true);
    setAutoDefault(false);
    setDefault(false);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

int GridBarButton::glyph_width() const
{
    return qCeil(style::icon_size(glyph_, kGlyphPointSize).width());
}

QSize GridBarButton::sizeHint() const
{
    ensurePolished();
    const int text_width = qCeil(QFontMetricsF(font()).horizontalAdvance(text()));
    const int width = kPadding.left() + glyph_width() + kGlyphGap + text_width + kPadding.right();
    // The height QCheckBox asks for with this font: the text's height through the style's
    // check-box sizing (which also fits the indicator).
    QStyleOptionButton option;
    option.initFrom(this);
    const QSize check_box = style()->sizeFromContents(QStyle::CT_CheckBox, &option, QSize(1, fontMetrics().height()), this);
    return QSize{width, std::max(check_box.height(), 1)};
}

QColor GridBarButton::glyph_colour() const
{
    const style::Theme& theme = style::theme();
    return isEnabled() ? theme.accent_line : theme.text_disabled;
}

QColor GridBarButton::text_colour() const
{
    const style::Theme& theme = style::theme();
    if (!isEnabled()) {
        return theme.text_disabled;
    }
    return underMouse() || isDown() ? theme.text : theme.text_secondary;
}

void GridBarButton::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    QStyleOptionButton option;
    initStyleOption(&option);

    // The face: the style's flat button (a fill under the mouse and while pressed).
    style()->drawControl(QStyle::CE_PushButtonBevel, &option, &painter, this);

    // The glyph and the text, left to right inside the padding, centred down the button.
    const QRect content = rect().marginsRemoved(kPadding);
    const int glyph = glyph_width();
    style::draw_icon(painter, QRectF(content.left(), 0, glyph, height()), glyph_, kGlyphPointSize, glyph_colour());
    painter.setFont(font());
    painter.setPen(text_colour());
    const int text_left = content.left() + glyph + kGlyphGap;
    painter.drawText(QRect(text_left, 0, std::max(0, width() - text_left), height()),
                     Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine, text());

    // Keyboard focus: the style's ring, only after a keyboard focus change.
    if (option.state.testFlag(QStyle::State_HasFocus) && option.state.testFlag(QStyle::State_KeyboardFocusChange)) {
        QStyleOptionFocusRect focus;
        focus.QStyleOption::operator=(option);
        focus.rect = rect();
        style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
    }
}

} // namespace wizz::app
