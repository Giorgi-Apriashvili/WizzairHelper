#include "trailing_row_layout.hpp"

#include <QWidget>
#include <QWidgetItem>

#include <algorithm>

namespace wizz::app {

namespace {

[[nodiscard]] bool visible(const QLayoutItem& item)
{
    return !item.isEmpty();
}

} // namespace

TrailingRowLayout::TrailingRowLayout(QWidget* parent) : QLayout(parent)
{
    setContentsMargins(0, 0, 0, 0);
    setSpacing(kSpacing);
}

TrailingRowLayout::~TrailingRowLayout() = default;

void TrailingRowLayout::add_widget(QWidget* widget)
{
    addChildWidget(widget);
    addItem(new QWidgetItem(widget));
}

void TrailingRowLayout::add_gap(int width)
{
    pending_gap_ += width;
}

void TrailingRowLayout::set_trailing(QWidget* widget)
{
    addChildWidget(widget);
    trailing_ = std::make_unique<QWidgetItem>(widget);
    invalidate();
}

void TrailingRowLayout::addItem(QLayoutItem* item)
{
    leading_.push_back(Entry{std::unique_ptr<QLayoutItem>(item), pending_gap_});
    pending_gap_ = 0;
    invalidate();
}

int TrailingRowLayout::count() const
{
    return static_cast<int>(leading_.size()) + (trailing_ ? 1 : 0);
}

QLayoutItem* TrailingRowLayout::itemAt(int index) const
{
    if (index >= 0 && index < static_cast<int>(leading_.size())) {
        return leading_[static_cast<std::size_t>(index)].item.get();
    }
    if (index == static_cast<int>(leading_.size()) && trailing_) {
        return trailing_.get();
    }
    return nullptr;
}

QLayoutItem* TrailingRowLayout::takeAt(int index)
{
    if (index >= 0 && index < static_cast<int>(leading_.size())) {
        QLayoutItem* item = leading_[static_cast<std::size_t>(index)].item.release();
        leading_.erase(leading_.begin() + index);
        invalidate();
        return item;
    }
    if (index == static_cast<int>(leading_.size()) && trailing_) {
        invalidate();
        return trailing_.release();
    }
    return nullptr;
}

Qt::Orientations TrailingRowLayout::expandingDirections() const
{
    return Qt::Horizontal;
}

bool TrailingRowLayout::hasHeightForWidth() const
{
    return true;
}

int TrailingRowLayout::leading_width() const
{
    int width = 0;
    bool first = true;
    for (const Entry& entry : leading_) {
        if (!visible(*entry.item)) {
            continue;
        }
        if (!first) {
            width += kSpacing;
        }
        width += entry.gap_before + entry.item->sizeHint().width();
        first = false;
    }
    return width;
}

int TrailingRowLayout::leading_height() const
{
    int height = 0;
    for (const Entry& entry : leading_) {
        if (visible(*entry.item)) {
            height = std::max(height, entry.item->sizeHint().height());
        }
    }
    return height;
}

QSize TrailingRowLayout::trailing_size() const
{
    return trailing_ && visible(*trailing_) ? trailing_->sizeHint() : QSize();
}

bool TrailingRowLayout::wraps_at(int width) const
{
    const QSize trailing = trailing_size();
    const int leading = leading_width();
    if (trailing.isEmpty() || leading == 0) {
        return false;
    }
    const QMargins m = contentsMargins();
    return leading + 2 * kSpacing + trailing.width() > width - m.left() - m.right();
}

int TrailingRowLayout::heightForWidth(int width) const
{
    const QMargins m = contentsMargins();
    const int leading = leading_height();
    const int trailing = trailing_size().height();
    const int height = wraps_at(width) ? leading + kSpacing + trailing : std::max(leading, trailing);
    return height + m.top() + m.bottom();
}

QSize TrailingRowLayout::minimumSize() const
{
    const QMargins m = contentsMargins();
    const int width = std::max(leading_width(), trailing_size().width());
    return QSize(width + m.left() + m.right(),
                 std::max(leading_height(), trailing_size().height()) + m.top() + m.bottom());
}

QSize TrailingRowLayout::sizeHint() const
{
    const QMargins m = contentsMargins();
    const QSize trailing = trailing_size();
    const int leading = leading_width();
    const int width = leading + (trailing.isEmpty() ? 0 : (leading > 0 ? 2 * kSpacing : 0) + trailing.width());
    return QSize(width + m.left() + m.right(), std::max(leading_height(), trailing.height()) + m.top() + m.bottom());
}

void TrailingRowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    const QRect area = rect.marginsRemoved(contentsMargins());
    const bool wrap = wraps_at(rect.width());
    const int line = wrap ? leading_height() : std::max(leading_height(), trailing_size().height());
    int x = area.left();
    bool first = true;
    for (const Entry& entry : leading_) {
        if (!visible(*entry.item)) {
            continue;
        }
        if (!first) {
            x += kSpacing;
        }
        x += entry.gap_before;
        const QSize size = entry.item->sizeHint();
        entry.item->setGeometry(QRect(x, area.top() + (line - size.height()) / 2, size.width(), size.height()));
        x += size.width();
        first = false;
    }
    if (trailing_ && visible(*trailing_)) {
        const QSize size = trailing_size();
        const int top = wrap ? area.top() + leading_height() + kSpacing : area.top() + (line - size.height()) / 2;
        trailing_->setGeometry(QRect(area.right() - size.width() + 1, top, size.width(), size.height()));
    }
}

} // namespace wizz::app
