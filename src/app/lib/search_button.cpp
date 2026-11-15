#include "search_button.hpp"

#include "ui_style.hpp"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionFocusRect>

namespace wizz::app {

namespace {

/// The flat button adapter's internal padding on each side (ButtonFlatAdapter
/// layout.paddingSize = 2; border size 0).
constexpr int kInternalPadding = 2;
/// Hover / pressed shades of the magenta (flat buttons darken or lighten their
/// BackColor while the mouse is over or down).
constexpr int kHoverLighter = 112;
constexpr int kPressedDarker = 115;

} // namespace

SearchButton::SearchButton(const QString& text, QWidget* parent) : QPushButton(text, parent)
{
    setFont(style::bold_form_font());
    setFlat(true);
    setAutoDefault(false);
    setDefault(false);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::StrongFocus);
}

QSize SearchButton::sizeHint() const
{
    ensurePolished();
    const QFontMetrics metrics(font());
    const QSize text_size = metrics.size(Qt::TextShowMnemonic, text());
    const int extra = 2 * kInternalPadding;
    const QSize hint(text_size.width() + kPadding.left() + kPadding.right() + extra,
                     text_size.height() + kPadding.top() + kPadding.bottom() + extra);
    return hint.expandedTo(kMinimumSize);
}

QSize SearchButton::minimumSizeHint() const
{
    return sizeHint();
}

QColor SearchButton::background_color() const
{
    const QColor magenta = style::wizz_magenta();
    if (!isEnabled()) {
        return magenta;
    }
    if (isDown()) {
        return magenta.darker(kPressedDarker);
    }
    if (underMouse()) {
        return magenta.lighter(kHoverLighter);
    }
    return magenta;
}

QColor SearchButton::text_color() const
{
    return isEnabled() ? QColor(Qt::white) : style::search_button_disabled_text();
}

void SearchButton::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    const QColor background = background_color();
    painter.fillRect(rect(), background);

    QRect text_rect = rect();
    if (isDown()) {
        text_rect.translate(1, 1);  // the flat adapter offsets pressed text by one pixel
    }
    painter.setFont(font());
    painter.setPen(text_color());
    painter.drawText(text_rect, Qt::AlignCenter | Qt::TextShowMnemonic, text());

    if (hasFocus()) {
        QStyleOptionFocusRect option;
        option.initFrom(this);
        option.rect = rect().adjusted(kInternalPadding, kInternalPadding, -kInternalPadding, -kInternalPadding);
        option.backgroundColor = background;
        style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter, this);
    }
}

void SearchButton::keyPressEvent(QKeyEvent* event)
{
    const bool plain = (event->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && plain) {
        if (isEnabled()) {
            click();
        }
        event->accept();
        return;
    }
    QPushButton::keyPressEvent(event);
}

} // namespace wizz::app
