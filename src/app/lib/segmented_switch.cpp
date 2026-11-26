#include "segmented_switch.hpp"

#include "modern_style.hpp"
#include "ui_style.hpp"

#include <QFocusEvent>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionButton>
#include <QStyleOptionFocusRect>
#include <QtMath>

#include <algorithm>
#include <cstddef>

namespace wizz::app {

namespace {

/// A divider is inset this far from the switch's top and bottom edges.
constexpr qreal kDividerInset = 6.0;

} // namespace

/// One segment: a radio button that paints its part of the switch.
class SegmentedSwitch::Segment final : public QRadioButton {
public:
    Segment(const QString& label, SegmentedSwitch& owner, int index)
        : QRadioButton(label, &owner), owner_(&owner), index_(index)
    {
        setAttribute(Qt::WA_Hover);  // QRadioButton repaints when the mouse enters or leaves
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setAccessibleName(label);
    }

    /// As wide as the switch's widest label plus the padding; as tall as the style makes a push
    /// button with this label (the app's look: one control height; High Contrast: the native one).
    [[nodiscard]] QSize sizeHint() const override
    {
        ensurePolished();
        const QFontMetricsF metrics(font());
        qreal widest = 0;
        for (const Segment* segment : owner_->segments_) {
            widest = std::max(widest, metrics.horizontalAdvance(segment->text()));
        }
        QStyleOptionButton option;
        option.initFrom(this);
        option.text = text();
        const QSize label = fontMetrics().size(Qt::TextShowMnemonic, text());
        const int height = style()->sizeFromContents(QStyle::CT_PushButton, &option, label, nullptr).height();
        return QSize{qCeil(widest) + 2 * kPadding, height};
    }
    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    /// The whole segment takes the mouse (a radio button only its circle and label).
    [[nodiscard]] bool hitButton(const QPoint& pos) const override { return rect().contains(pos); }

    void focusInEvent(QFocusEvent* event) override
    {
        QRadioButton::focusInEvent(event);
        owner_->keep_tab_stop_on_current();
    }

    void focusOutEvent(QFocusEvent* event) override
    {
        QRadioButton::focusOutEvent(event);
        owner_->keep_tab_stop_on_current();  // back to the selected segment
    }

    void paintEvent(QPaintEvent* /*event*/) override
    {
        const style::Theme& t = style::theme();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        // The whole switch's rounded outline, in this segment's coordinates.
        const QRectF whole(QPointF(-pos().x(), -pos().y()), QSizeF(owner_->size()));
        QPainterPath outline;
        outline.addRoundedRect(whole, ModernStyle::kRadius, ModernStyle::kRadius);

        painter.setClipPath(outline);
        painter.fillRect(rect(), owner_->fill_colour(index_));
        const bool selected = isChecked();
        const auto next = static_cast<std::size_t>(index_) + 1;
        if (next < owner_->segments_.size() && !selected && !owner_->segments_[next]->isChecked()) {
            // A divider between two unselected segments (the selection's fill parts the others).
            painter.setPen(QPen(t.border, 1.0));
            const qreal x = width() - 0.5;
            painter.drawLine(QPointF(x, kDividerInset), QPointF(x, height() - kDividerInset));
        }
        if (selected && !t.high_contrast) {
            // The accent pill along the bottom, like a selected tab's (High Contrast: the fill).
            painter.setPen(Qt::NoPen);
            painter.setBrush(isEnabled() ? t.accent_line : t.text_disabled);
            painter.drawRoundedRect(QRectF(kPillInset, height() - 1 - kPillHeight, width() - 2 * kPillInset, kPillHeight),
                                    kPillHeight / 2.0, kPillHeight / 2.0);
        }
        painter.setClipping(false);

        // The frame of the whole switch: its part in this segment.
        painter.setPen(QPen(t.input_border, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(whole.adjusted(0.5, 0.5, -0.5, -0.5), ModernStyle::kRadius, ModernStyle::kRadius);

        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setFont(font());
        painter.setPen(owner_->label_colour(index_));
        painter.drawText(rect(), Qt::AlignCenter | Qt::TextSingleLine, text());

        // Keyboard focus: the style's ring (the accent; a dotted frame in High Contrast), only after a
        // keyboard focus change.
        QStyleOptionButton option;
        initStyleOption(&option);
        if (option.state.testFlag(QStyle::State_HasFocus) && option.state.testFlag(QStyle::State_KeyboardFocusChange)) {
            QStyleOptionFocusRect focus;
            focus.QStyleOption::operator=(option);
            focus.rect = rect().adjusted(2, 2, -2, -2);
            // The plain style (High Contrast) dots the frame in the inverse of this colour; without
            // it the painter's background, the Button colour, which can be all but invisible on the
            // Highlight of a selected segment (Aquatic: #DFDFDF dots on #8EE3F0).
            focus.backgroundColor = owner_->fill_colour(index_);
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
        }
    }

private:
    SegmentedSwitch* owner_;
    int index_;
};

SegmentedSwitch::SegmentedSwitch(const QStringList& labels, QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    for (const QString& label : labels) {
        const int index = count();
        auto* segment = new Segment(label, *this, index);
        segments_.push_back(segment);
        row->addWidget(segment);
        connect(segment, &QRadioButton::toggled, this, [this, index](bool checked) {
            for (Segment* each : segments_) {
                each->update();  // the dividers beside the selection move with it
            }
            if (checked) {
                keep_tab_stop_on_current();
                Q_EMIT current_changed(index);
            }
        });
    }
    if (!segments_.empty()) {
        segments_.front()->setChecked(true);
    }
    keep_tab_stop_on_current();
}

QRadioButton* SegmentedSwitch::segment(int index) const
{
    return index >= 0 && index < count() ? segments_[static_cast<std::size_t>(index)] : nullptr;
}

int SegmentedSwitch::current() const
{
    const auto selected = std::ranges::find_if(segments_, [](const Segment* segment) { return segment->isChecked(); });
    return selected == segments_.end() ? -1 : static_cast<int>(selected - segments_.begin());
}

void SegmentedSwitch::set_current(int index)
{
    if (QRadioButton* chosen = segment(index); chosen != nullptr) {
        chosen->setChecked(true);  // auto-exclusive: the others clear; toggled() reports it
    }
}

QColor SegmentedSwitch::fill_colour(int index) const
{
    const style::Theme& t = style::theme();
    const QRadioButton* button = segment(index);
    if (button == nullptr || !button->isEnabled()) {
        return t.input_disabled;
    }
    if (button->isChecked()) {
        return t.selection;
    }
    if (button->isDown()) {
        return t.button_pressed;
    }
    return button->underMouse() ? t.button_hover : t.button;
}

QColor SegmentedSwitch::label_colour(int index) const
{
    const style::Theme& t = style::theme();
    const QRadioButton* button = segment(index);
    if (button == nullptr || !button->isEnabled()) {
        return t.text_disabled;
    }
    if (button->isChecked()) {
        return t.selection_text;
    }
    return button->underMouse() || button->isDown() ? t.text : t.text_secondary;  // like a tab's label
}

void SegmentedSwitch::keep_tab_stop_on_current()
{
    // Qt gives the Tab stop of a radio group to the button that last had the focus, and to every
    // button until one had it; a Windows radio group's stop is its selected button. While a segment
    // has the focus it is the stop, as in Qt, so Tab and Shift+Tab leave the switch at once also from
    // a segment the focus reached without selecting it (a right-click, a press dragged off).
    const auto focused = std::ranges::find_if(segments_, [](const Segment* each) { return each->hasFocus(); });
    for (Segment* each : segments_) {
        const bool stop = focused != segments_.end() ? each == *focused : each->isChecked();
        each->setFocusPolicy(stop ? Qt::StrongFocus : Qt::ClickFocus);
    }
}

} // namespace wizz::app
