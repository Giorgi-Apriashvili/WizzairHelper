#include "section_card.hpp"

#include "modern_style.hpp"
#include "ui_style.hpp"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace wizz::app {

namespace {

constexpr int kHeaderGap = 8;
constexpr int kBodyGap = 6;

void set_text_colour(QWidget& widget, const QColor& colour)
{
    QPalette palette = widget.palette();
    palette.setColor(QPalette::WindowText, colour);
    widget.setPalette(palette);
}

} // namespace

GlyphLabel::GlyphLabel(char16_t glyph, bool mirrored, QWidget* parent, QColor colour, qreal point_size)
    : QWidget(parent), glyph_(glyph), mirrored_(mirrored), colour_(colour), point_size_(point_size)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFocusPolicy(Qt::NoFocus);
}

QSize GlyphLabel::sizeHint() const
{
    const QSizeF ink = style::icon_size(glyph_, point_size_);
    const int side = std::max(16, static_cast<int>(std::ceil(std::max(ink.width(), ink.height()))) + 2);
    return {side, side};
}

void GlyphLabel::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    style::draw_icon(painter, QRectF(rect()), glyph_, point_size_,
                     colour_.isValid() ? colour_ : style::theme().accent_line, mirrored_);
}

SectionCard::SectionCard(const QString& title, char16_t glyph, bool mirror_glyph, QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kPadding);
    outer->setSpacing(kBodyGap);

    header_ = new QHBoxLayout;
    header_->setContentsMargins(0, 0, 0, 0);
    header_->setSpacing(kHeaderGap);
    auto* icon = new GlyphLabel(glyph, mirror_glyph, this);
    title_ = new QLabel(title, this);
    title_->setTextFormat(Qt::PlainText);
    title_->setFont(style::caption_font());
    set_text_colour(*title_, style::theme().text_secondary);
    header_->addWidget(icon, 0, Qt::AlignVCenter);
    header_->addWidget(title_, 0, Qt::AlignVCenter);
    header_->addStretch(1);
    outer->addLayout(header_);

    body_ = new QVBoxLayout;
    body_->setContentsMargins(0, 0, 0, 0);
    body_->setSpacing(8);
    outer->addLayout(body_);
    outer->addStretch(1);
}

QLabel* SectionCard::set_hint(const QString& text)
{
    if (hint_ == nullptr) {
        hint_ = new QLabel(this);
        hint_->setTextFormat(Qt::PlainText);
        hint_->setFont(style::hint_font());
        set_text_colour(*hint_, style::theme().text_muted);
        header_->insertWidget(2, hint_, 0, Qt::AlignVCenter);
        header_->insertSpacing(2, 6);
    }
    hint_->setText(text);
    return hint_;
}

void SectionCard::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(style::theme().border, 1.0));
    painter.setBrush(style::theme().surface);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), ModernStyle::kCardRadius,
                            ModernStyle::kCardRadius);
}

} // namespace wizz::app
