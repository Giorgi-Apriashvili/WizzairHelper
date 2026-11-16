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

constexpr int kIconGap = 8;
constexpr int kHintGap = 6;

/// The height a one-line plain label in `font` asks for.
[[nodiscard]] int label_height(const QFont& font)
{
    QLabel probe(QStringLiteral("Ag"));
    probe.setTextFormat(Qt::PlainText);
    probe.setFont(font);
    return probe.sizeHint().height();
}

} // namespace

GlyphLabel::GlyphLabel(char16_t glyph, bool mirrored, QWidget* parent, Tone tone, qreal point_size)
    : QWidget(parent), glyph_(glyph), mirrored_(mirrored), tone_(tone), point_size_(point_size)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFocusPolicy(Qt::NoFocus);
}

QSize GlyphLabel::sizeHint() const
{
    const QSizeF ink = style::icon_size(glyph_, point_size_);
    const int side = std::max(14, static_cast<int>(std::ceil(std::max(ink.width(), ink.height()))) + 2);
    return {side, side};
}

void GlyphLabel::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    const style::Theme& theme = style::theme();
    style::draw_icon(painter, QRectF(rect()), glyph_, point_size_,
                     tone_ == Tone::muted ? theme.text_muted : theme.accent_line, mirrored_);
}

SectionCard::SectionCard(const QString& title, char16_t glyph, bool mirror_glyph, QWidget* parent) : QWidget(parent)
{
    setAccessibleName(title);  // screen readers name the group the fields are in
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kPadding);
    outer->setSpacing(kHeaderGap);

    header_ = new QHBoxLayout;
    header_->setContentsMargins(0, 0, 0, 0);
    header_->setSpacing(kIconGap);
    auto* icon = new GlyphLabel(glyph, mirror_glyph, this, GlyphLabel::Tone::accent, 10.0);
    title_ = new QLabel(title, this);
    title_->setTextFormat(Qt::PlainText);
    title_->setFont(style::caption_font());
    style::set_text_tone(*title_, style::TextTone::secondary);
    header_->addWidget(icon, 0, Qt::AlignVCenter);
    header_->addWidget(title_, 0, Qt::AlignVCenter);
    header_->addStretch(1);
    // Every caption row is as tall as one with a hint, so the bodies of two cards side by
    // side start on one line (and their search buttons end on one), hint or not.
    header_->addStrut(std::max({icon->sizeHint().height(), title_->sizeHint().height(),
                                label_height(style::hint_font())}));
    outer->addLayout(header_);

    body_ = new QVBoxLayout;
    body_->setContentsMargins(0, 0, 0, 0);
    body_->setSpacing(kRowGap);
    outer->addLayout(body_);
    outer->addStretch(1);
}

QLabel* SectionCard::set_hint(const QString& text)
{
    if (hint_ == nullptr) {
        hint_ = new QLabel(this);
        hint_->setTextFormat(Qt::PlainText);
        hint_->setFont(style::hint_font());
        style::set_text_tone(*hint_, style::TextTone::muted);
        header_->insertWidget(2, hint_, 0, Qt::AlignVCenter);
        header_->insertSpacing(2, kHintGap);
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
