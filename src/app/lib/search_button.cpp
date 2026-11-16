#include "search_button.hpp"

#include "modern_style.hpp"
#include "ui_style.hpp"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace wizz::app {

namespace {

constexpr int kGlyphGap = 8;
constexpr qreal kGlyphPointSize = 9.5;

} // namespace

SearchButton::SearchButton(const QString& text, QWidget* parent) : QPushButton(text, parent)
{
    setFont(style::bold_form_font());
    setFlat(true);
    setAutoDefault(false);
    setDefault(false);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
}

void SearchButton::set_glyph(char16_t glyph)
{
    glyph_ = glyph;
    updateGeometry();
    update();
}

int SearchButton::glyph_width() const
{
    if (glyph_ == 0) {
        return 0;
    }
    return static_cast<int>(std::ceil(style::icon_size(glyph_, kGlyphPointSize).width())) + kGlyphGap;
}

QSize SearchButton::sizeHint() const
{
    ensurePolished();
    const QFontMetrics metrics(font());
    const QSize text_size = metrics.size(Qt::TextShowMnemonic, text());
    const QSize hint(glyph_width() + text_size.width() + kPadding.left() + kPadding.right(),
                     std::max(ModernStyle::control_height(QFontMetrics(style::form_font())),
                              text_size.height() + kPadding.top() + kPadding.bottom()));
    return hint.expandedTo(kMinimumSize);
}

QSize SearchButton::minimumSizeHint() const
{
    return sizeHint();
}

QColor SearchButton::background_color() const
{
    const style::Theme& theme = style::theme();
    if (theme.high_contrast) {
        return isEnabled() ? theme.accent : theme.window;  // system Highlight / Window only
    }
    if (!isEnabled()) {
        return theme.input_disabled;  // like every disabled control, in both schemes
    }
    if (isDown()) {
        return theme.accent_pressed;
    }
    if (underMouse()) {
        return theme.accent_hover;
    }
    return theme.accent;
}

QColor SearchButton::text_color() const
{
    const style::Theme& theme = style::theme();
    return isEnabled() ? theme.on_accent : theme.text_disabled;  // High Contrast: HighlightedText / GrayText
}

void SearchButton::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF face = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const style::Theme& theme = style::theme();
    // Disabled: an outline keeps the shape visible on the card (High Contrast: GrayText).
    painter.setPen(isEnabled() ? QPen(Qt::NoPen)
                               : QPen(theme.high_contrast ? theme.text_disabled : theme.input_border, 1.0));
    painter.setBrush(background_color());
    painter.drawRoundedRect(face, ModernStyle::kRadius, ModernStyle::kRadius);

    // Icon and text centred together.
    const QFontMetrics metrics(font());
    const int text_width = metrics.horizontalAdvance(text());
    const int content = glyph_width() + text_width;
    int x = (width() - content) / 2;
    painter.setPen(text_color());
    if (glyph_ != 0) {
        const int advance = glyph_width() - kGlyphGap;
        style::draw_icon(painter, QRectF(x, 0, advance, height()), glyph_, kGlyphPointSize, text_color());
        x += advance + kGlyphGap;
    }
    painter.setFont(font());
    painter.drawText(QRect(x, 0, text_width + 1, height()), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextShowMnemonic,
                     text());

    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        QColor ring = theme.on_accent;
        ring.setAlpha(theme.high_contrast ? 255 : 230);
        painter.setPen(QPen(ring, 1.5));
        painter.drawRoundedRect(QRectF(rect()).adjusted(2.75, 2.75, -2.75, -2.75), ModernStyle::kRadius - 2,
                                ModernStyle::kRadius - 2);
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
