#include "elided_label.hpp"

#include <QEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QPainter>
#include <QToolTip>
#include <QtMath>

namespace wizz::app {

ElidedLabel::ElidedLabel(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
}

void ElidedLabel::setText(const QString& text)
{
    if (text_ == text) {
        return;
    }
    text_ = text;
    update();
}

void ElidedLabel::setAlignment(Qt::Alignment alignment)
{
    if (alignment_ == alignment) {
        return;
    }
    alignment_ = alignment;
    update();
}

void ElidedLabel::set_compact_font(const std::optional<QFont>& font)
{
    if (compact_ == font) {
        return;
    }
    compact_ = font;
    update();
}

QFont ElidedLabel::painted_font() const
{
    if (compact_ &&
        fontMetrics().elidedText(text_, Qt::ElideRight, contentsRect().width(), Qt::TextSingleLine) != text_) {
        return *compact_;
    }
    return font();
}

QString ElidedLabel::elided_text() const
{
    return QFontMetrics(painted_font())
        .elidedText(text_, Qt::ElideRight, contentsRect().width(), Qt::TextSingleLine);
}

bool ElidedLabel::is_elided() const
{
    return elided_text() != text_;
}

QString ElidedLabel::tooltip_text() const
{
    return is_elided() ? text_ : QString{};
}

QSize ElidedLabel::sizeHint() const
{
    // Rounded up: without hinting (D-58) glyphs sit at fractional positions, and a width
    // rounded down would cut the last fraction of a pixel with "…".
    const QFontMetricsF metrics(font());
    const QMargins margins = contentsMargins();
    return {qCeil(metrics.horizontalAdvance(text_)) + margins.left() + margins.right(),
            qCeil(metrics.height()) + margins.top() + margins.bottom()};
}

QSize ElidedLabel::minimumSizeHint() const
{
    const QMargins margins = contentsMargins();
    return {margins.left() + margins.right(), qCeil(QFontMetricsF(font()).height()) + margins.top() + margins.bottom()};
}

bool ElidedLabel::event(QEvent* event)
{
    if (event->type() == QEvent::ToolTip) {
        // Decided when the user hovers, for the width and text of that moment.
        if (const QString tooltip = tooltip_text(); !tooltip.isEmpty()) {
            QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(), tooltip, this);
        } else {
            QToolTip::hideText();
            event->ignore();
        }
        return true;
    }
    return QWidget::event(event);
}

void ElidedLabel::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setPen(palette().color(foregroundRole()));
    painter.setFont(painted_font());
    painter.drawText(contentsRect(), static_cast<int>(alignment_) | Qt::TextSingleLine, elided_text());
}

void ElidedLabel::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        updateGeometry();
    }
}

} // namespace wizz::app
