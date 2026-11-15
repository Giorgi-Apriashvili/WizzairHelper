#pragma once
// Cell painting of both result grids, as the WinForms DataGridView paints
// them (01 §8.3, §9.1): a selected row uses the system highlight colours
// (SystemColors.Highlight / HighlightText) in every style — or the model's own
// selection colour (the flight grid's cheapest rows: RGB 40,140,70) — whether
// or not the grid has the focus; there is no hover effect. Unselected rows
// keep the model's Background and Foreground roles (the cheapest rows' green,
// the grey of unavailable fares). A cell whose text does not fit shows it in
// a tooltip (DataGridView.ShowCellToolTips).
//
// Styles differ in how they draw item-view selection (the Windows 11 style
// draws a faint fill with an accent bar in every cell of a table row), so the
// delegate paints the selection as the cell background itself.

#include <QStyledItemDelegate>

namespace wizz::app {

class GridRowDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    /// Optional model role: the QBrush of a row's selection background (instead of Highlight).
    static constexpr int SelectionBackgroundRole = Qt::UserRole + 100;

    using QStyledItemDelegate::QStyledItemDelegate;

    /// Public so tests can check the option a cell is painted with.
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override;
    /// Tooltips show the full text of truncated cells only.
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
                   const QModelIndex& index) override;

    /// True when the cell text of `option` (after initStyleOption) is wider than its rect.
    [[nodiscard]] static bool text_truncated(const QStyleOptionViewItem& option, const QWidget* widget);
};

} // namespace wizz::app
