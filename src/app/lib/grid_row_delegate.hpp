#pragma once
// Cell painting of both result grids (01 §8.3, §9.1) in the look of DECISIONS
// D-53: rows are separated by a thin line instead of a cell grid; a
// selected row takes a soft accent tint (or the model's own selection colour:
// the cheapest rows' deeper green) and a 3 px bar at the left edge of the view:
// the accent while the grid has the keyboard focus, the muted text colour
// otherwise. A focused grid's current row has the accent bar even when it is not
// selected, so the focus always shows. There is no hover effect. Unselected rows keep the
// model's Background and Foreground roles (the cheapest rows' green, the muted
// text of unavailable fares). A cell whose text does not fit shows it in a
// tooltip (DataGridView.ShowCellToolTips). Painting allocates nothing per
// cell: the selection brush is kept and renewed when the look changes.

#include <QBrush>
#include <QStyledItemDelegate>

namespace wizz::app {

class GridRowDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    /// Optional model role: the QBrush of a row's selection background (instead of Highlight).
    static constexpr int SelectionBackgroundRole = Qt::UserRole + 100;

    explicit GridRowDelegate(QObject* parent = nullptr);

    /// Public so tests can check the option a cell is painted with.
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override;
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    /// Tooltips show the full text of truncated cells only.
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option,
                   const QModelIndex& index) override;

    /// True when the cell text of `option` (after initStyleOption) is wider than its rect.
    [[nodiscard]] static bool text_truncated(const QStyleOptionViewItem& option, const QWidget* widget);

private:
    QBrush selection_;
};

} // namespace wizz::app
