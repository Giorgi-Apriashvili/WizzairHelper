#include "panel_progress_bar.hpp"

#include "ui_style.hpp"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cstdint>

namespace wizz::app {

namespace {

/// Thickness of the rounded track (logical pixels).
constexpr qreal kTrackHeight = 6.0;
/// The marquee segment: its share of the track and one sweep's duration.
constexpr qreal kMarqueeShare = 0.28;
constexpr qint64 kMarqueePeriodMs = 1600;

} // namespace

PanelProgressBar::PanelProgressBar(QWidget* parent) : QProgressBar(parent)
{
    fill_ = style::theme().accent;
    clock_.start();
}

void PanelProgressBar::set_live(bool live)
{
    if (live_ != live) {
        live_ = live;
        update();
    }
}

void PanelProgressBar::set_fill(const QColor& colour)
{
    if (fill_ != colour) {
        fill_ = colour;
        update();
    }
}

void PanelProgressBar::paintEvent(QPaintEvent* /*event*/)
{
    // Painted here, not by the style: a rounded track and fill that never move on their
    // own. Only a live marquee advances, and only when the busy panel's timer repaints it.
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF track(0, (height() - kTrackHeight) / 2.0, width(), kTrackHeight);
    const qreal radius = kTrackHeight / 2.0;
    QPainterPath shape;
    shape.addRoundedRect(track, radius, radius);
    painter.fillPath(shape, style::theme().track);
    painter.setClipPath(shape);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill_);

    const bool marquee = minimum() == 0 && maximum() == 0;
    if (marquee) {
        if (!live_) {
            return;  // End always switches to Continuous; an idle marquee shows an empty track
        }
        const qreal segment = track.width() * kMarqueeShare;
        const qreal phase =
            static_cast<qreal>(clock_.elapsed() % kMarqueePeriodMs) / static_cast<qreal>(kMarqueePeriodMs);
        const qreal x = track.left() - segment + (track.width() + segment) * phase;
        painter.drawRoundedRect(QRectF(x, track.top(), segment, kTrackHeight), radius, radius);
        return;
    }

    const std::int64_t span = std::int64_t{maximum()} - minimum();
    const std::int64_t done = std::clamp<std::int64_t>(std::int64_t{value()} - minimum(), 0, span);
    if (span <= 0 || done <= 0) {
        return;
    }
    const qreal width = track.width() * static_cast<qreal>(done) / static_cast<qreal>(span);
    const bool from_right = (layoutDirection() == Qt::RightToLeft) != invertedAppearance();
    const QRectF fill = from_right ? QRectF(track.right() - width, track.top(), width, kTrackHeight)
                                   : QRectF(track.left(), track.top(), width, kTrackHeight);
    painter.drawRoundedRect(fill, radius, radius);
}

} // namespace wizz::app
