#include "grid_row_delegate.hpp"

#include "grid_colors.hpp"
#include "ui_style.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QHelpEvent>
#include <QPainter>
#include <QStyle>
#include <QToolTip>

namespace wizz::app {

namespace {

constexpr int kFocusBarWidth = 3;

} // namespace

void GridRowDelegate::initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    option->state.setFlag(QStyle::State_MouseOver, false);  // DataGridView rows have no hover effect
    option->state.setFlag(QStyle::State_HasFocus, false);   // no per-cell focus frame: the row shows it
    if (!option->state.testFlag(QStyle::State_Selected)) {
        return;
    }
    const QVariant custom = index.data(SelectionBackgroundRole);
    const QColor text = grid_colors::highlight_text();
    option->state.setFlag(QStyle::State_Selected, false);
    option->backgroundBrush = custom.isValid() ? custom.value<QBrush>() : QBrush{grid_colors::highlight()};
    option->palette.setColor(QPalette::Text, text);
    option->palette.setColor(QPalette::HighlightedText, text);
}

void GridRowDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QStyledItemDelegate::paint(painter, option, index);
    const QRect r = option.rect;
    painter->fillRect(QRect(r.left(), r.bottom(), r.width(), 1), style::theme().row_separator);
    const auto* view = qobject_cast<const QAbstractItemView*>(option.widget);
    if (view != nullptr && option.state.testFlag(QStyle::State_Selected) && view->hasFocus() &&
        r.left() <= 0 && r.right() >= 0) {
        // The keyboard focus is on this grid: an accent bar at the row's visible left edge.
        painter->fillRect(QRect(0, r.top(), kFocusBarWidth, r.height()), style::theme().accent_line);
    }
}

bool GridRowDelegate::helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
                                const QModelIndex& index)
{
    if (event == nullptr || view == nullptr || event->type() != QEvent::ToolTip || !index.isValid()) {
        return QStyledItemDelegate::helpEvent(event, view, option, index);
    }
    QStyleOptionViewItem cell = option;
    initStyleOption(&cell, index);
    if (!cell.text.isEmpty() && text_truncated(cell, view)) {
        QToolTip::showText(event->globalPos(), cell.text, view->viewport(), cell.rect);
    } else {
        QToolTip::hideText();
    }
    return true;
}

bool GridRowDelegate::text_truncated(const QStyleOptionViewItem& option, const QWidget* widget)
{
    const QStyle* style = widget != nullptr ? widget->style() : QApplication::style();
    // QCommonStyle's item text margin: PM_FocusFrameHMargin + 1 on each side.
    const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, &option, widget) + 1;
    return option.fontMetrics.horizontalAdvance(option.text) + 2 * margin > option.rect.width();
}

} // namespace wizz::app
