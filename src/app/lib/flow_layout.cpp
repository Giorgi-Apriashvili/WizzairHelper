#include "flow_layout.hpp"

#include <QSpacerItem>
#include <QWidget>

#include <algorithm>

namespace wizz::app {

namespace {

/// A hidden widget takes no space; spacer items (gaps) always do.
[[nodiscard]] bool takes_space(QLayoutItem& item)
{
    return item.widget() == nullptr ? item.spacerItem() != nullptr || !item.isEmpty() : !item.isEmpty();
}

[[nodiscard]] QSize outer_size(const QLayoutItem& item, const QMargins& margins)
{
    const QSize size = item.sizeHint();
    return {margins.left() + size.width() + margins.right(), margins.top() + size.height() + margins.bottom()};
}

} // namespace

FlowLayout::FlowLayout(QWidget* parent) : QLayout(parent)
{
    setContentsMargins(0, 0, 0, 0);
    setSpacing(0);
}

FlowLayout::~FlowLayout() = default;

void FlowLayout::add_widget(QWidget* widget, QMargins margins)
{
    addWidget(widget);  // → addItem() with the default margins
    entries_.back().margins = margins;
}

void FlowLayout::add_gap(int width)
{
    addItem(new QSpacerItem(width, 1, QSizePolicy::Fixed, QSizePolicy::Fixed));
    entries_.back().margins = QMargins{};
}

QMargins FlowLayout::item_margins(int index) const
{
    if (index < 0 || index >= count()) {
        return {};
    }
    return entries_[static_cast<std::size_t>(index)].margins;
}

int FlowLayout::line_count(int width) const
{
    return run(QRect(0, 0, width, 0), false).lines;
}

void FlowLayout::addItem(QLayoutItem* item)
{
    entries_.push_back(Entry{std::unique_ptr<QLayoutItem>(item), kDefaultItemMargins});
    invalidate();
}

int FlowLayout::count() const
{
    return static_cast<int>(entries_.size());
}

QLayoutItem* FlowLayout::itemAt(int index) const
{
    if (index < 0 || index >= count()) {
        return nullptr;
    }
    return entries_[static_cast<std::size_t>(index)].item.get();
}

QLayoutItem* FlowLayout::takeAt(int index)
{
    if (index < 0 || index >= count()) {
        return nullptr;
    }
    const auto position = entries_.begin() + index;
    QLayoutItem* item = position->item.release();
    entries_.erase(position);
    invalidate();
    return item;
}

Qt::Orientations FlowLayout::expandingDirections() const
{
    return {};
}

bool FlowLayout::hasHeightForWidth() const
{
    return true;
}

int FlowLayout::heightForWidth(int width) const
{
    return run(QRect(0, 0, width, 0), false).height;
}

QSize FlowLayout::minimumSize() const
{
    // No minimum width: like a docked FlowLayoutPanel, an item wider than the row
    // overflows its own line. (A minimum width would make a parent box layout ask
    // heightForWidth for a width the row never gets.) The height is the tallest item.
    int height = 0;
    for (const Entry& entry : entries_) {
        if (takes_space(*entry.item)) {
            height = std::max(height, outer_size(*entry.item, entry.margins).height());
        }
    }
    const QMargins margins = contentsMargins();
    return {margins.left() + margins.right(), margins.top() + height + margins.bottom()};
}

QSize FlowLayout::sizeHint() const
{
    // Everything on one line.
    int width = 0;
    int height = 0;
    for (const Entry& entry : entries_) {
        if (takes_space(*entry.item)) {
            const QSize outer = outer_size(*entry.item, entry.margins);
            width += outer.width();
            height = std::max(height, outer.height());
        }
    }
    return QSize(width, height).grownBy(contentsMargins());
}

void FlowLayout::setGeometry(const QRect& rect)
{
    QLayout::setGeometry(rect);
    run(rect, true);
}

FlowLayout::Pass FlowLayout::run(const QRect& rect, bool apply) const
{
    const QMargins margins = contentsMargins();
    const QRect area = rect.marginsRemoved(margins);
    const int right = area.x() + area.width();  // one past the last usable pixel
    int x = area.x();
    int y = area.y();
    int line_height = 0;
    int lines = 0;
    bool line_started = false;
    for (const Entry& entry : entries_) {
        QLayoutItem& item = *entry.item;
        if (!takes_space(item)) {
            continue;
        }
        const QSize size = item.sizeHint();
        const QSize outer = outer_size(item, entry.margins);
        if (line_started && x + outer.width() > right) {
            x = area.x();
            y += line_height;
            line_height = 0;
            line_started = false;
        }
        if (!line_started) {
            ++lines;
            line_started = true;
        }
        if (apply) {
            item.setGeometry(QRect(QPoint(x + entry.margins.left(), y + entry.margins.top()), size));
        }
        x += outer.width();
        line_height = std::max(line_height, outer.height());
    }
    return Pass{y + line_height - rect.y() + margins.bottom(), lines};
}

} // namespace wizz::app
