#include "grid_row_delegate.hpp"

#include "grid_colors.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QHelpEvent>
#include <QStyle>
#include <QToolTip>

namespace wizz::app {

void GridRowDelegate::initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const
{
    QStyledItemDelegate::initStyleOption(option, index);
    option->state.setFlag(QStyle::State_MouseOver, false);  // DataGridView rows have no hover effect
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
