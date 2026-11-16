#include "panel_progress_bar.hpp"

#include <QStyle>
#include <QStyleOptionProgressBar>
#include <QStylePainter>

#include <algorithm>
#include <cstdint>

namespace wizz::app {

PanelProgressBar::PanelProgressBar(QWidget* parent) : QProgressBar(parent) {}

void PanelProgressBar::set_live(bool live)
{
    if (live_ != live) {
        live_ = live;
        update();  // the next paint starts or stops the style's animation
    }
}

void PanelProgressBar::paintEvent(QPaintEvent* event)
{
    const bool marquee = minimum() == 0 && maximum() == 0;
    if (live_ || marquee) {  // End always switches to Continuous, so an idle marquee does not occur
        QProgressBar::paintEvent(event);
        return;
    }

    // Idle: the same layers QCommonStyle draws for CE_ProgressBar, without motion.
    QStylePainter painter(this);
    QStyleOptionProgressBar option;
    initStyleOption(&option);
    QStyle* const s = style();

    QStyleOptionProgressBar groove = option;
    groove.rect = s->subElementRect(QStyle::SE_ProgressBarGroove, &option, this);
    painter.drawControl(QStyle::CE_ProgressBarGroove, groove);

    QStyleOptionProgressBar contents = option;
    contents.rect = s->subElementRect(QStyle::SE_ProgressBarContents, &option, this);
    const std::int64_t span = std::int64_t{option.maximum} - option.minimum;
    const std::int64_t done = std::clamp<std::int64_t>(std::int64_t{option.progress} - option.minimum, 0, span);
    if (span <= 0 || done <= 0) {
        contents.progress = contents.minimum;  // empty: no fill, and the style stops any animation
        painter.drawControl(QStyle::CE_ProgressBarContents, contents);
        return;
    }

    // A full bar is drawn without the glow; clip it to the part that is really done.
    QRect fill = contents.rect;
    const int width = static_cast<int>((std::int64_t{fill.width()} * done) / span);
    const bool from_right = (option.direction == Qt::RightToLeft) != option.invertedAppearance;
    if (from_right) {
        fill.setLeft(fill.right() - width + 1);
    } else {
        fill.setWidth(width);
    }
    contents.progress = contents.maximum;
    painter.save();
    painter.setClipRect(fill);
    painter.drawControl(QStyle::CE_ProgressBarContents, contents);
    painter.restore();
}

} // namespace wizz::app
