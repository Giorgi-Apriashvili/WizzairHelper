#include "grid_table_view.hpp"

#include "grid_row_delegate.hpp"
#include "ui_style.hpp"

#include <QApplication>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOptionHeader>

#include <algorithm>

namespace wizz::app {

namespace {

[[nodiscard]] bool is_enter(const QKeyEvent& event) noexcept
{
    return (event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter) &&
           (event.modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
}

[[nodiscard]] bool is_navigation_key(int key) noexcept
{
    switch (key) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
        return true;
    default:
        return false;
    }
}

} // namespace

GridTableView::GridTableView(QWidget* parent) : QTableView(parent)
{
    setEditTriggers(NoEditTriggers);
    setSelectionBehavior(SelectRows);
    setSelectionMode(SingleSelection);
    setFrameShape(QFrame::NoFrame);
    setWordWrap(false);
    setShowGrid(false);  // rows are separated by the delegate's line, columns by space
    setFont(style::data_font());  // denser cells; the header keeps the UI size (below)
    apply_theme();
    style::on_theme_change(this, [this] { apply_theme(); });
    setCornerButtonEnabled(false);
    setSortingEnabled(false);              // header clicks sort through ui_logic (01 §8.4)
    setVerticalScrollMode(ScrollPerItem);  // the scroll value is the first displayed row (01 §8.6)
    setHorizontalScrollMode(ScrollPerPixel);
    setContextMenuPolicy(Qt::DefaultContextMenu);

    QHeaderView* rows = verticalHeader();
    rows->hide();  // no row headers
    rows->setSectionResizeMode(QHeaderView::Fixed);

    QHeaderView* columns = horizontalHeader();
    columns->setSectionsMovable(false);  // no column reordering
    columns->setSectionsClickable(true);
    columns->setSectionResizeMode(QHeaderView::Interactive);
    columns->setStretchLastSection(false);
    columns->setHighlightSections(false);
    columns->setFont(style::bold_form_font());  // semibold, the UI size
    // WinForms header cells: MiddleLeft, wrapped text.
    columns->setDefaultAlignment(Qt::Alignment::fromInt(Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap));
    columns->setTextElideMode(Qt::ElideNone);
    columns->setSortIndicatorShown(true);
    columns->setSortIndicator(-1, Qt::AscendingOrder);  // the default sort shows no glyph
    // The grid's menu also opens over the header (01 §8.9 "anywhere in the grid").
    columns->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(columns, &QHeaderView::customContextMenuRequested, this,
            [this, columns](QPoint pos) { Q_EMIT context_menu_requested(columns->mapToGlobal(pos)); });
    connect(columns, &QHeaderView::sectionResized, this, [this](int logical, int, int) {
        if (logical != fill_column_) {
            fit_fill_column();
        }
        fit_header_height();
    });
    connect(columns, &QHeaderView::sortIndicatorChanged, this, &GridTableView::fit_header_height);

    setItemDelegate(new GridRowDelegate(this));
    apply_row_height();
}

void GridTableView::setModel(QAbstractItemModel* model)
{
    disconnect(header_texts_);
    QTableView::setModel(model);
    if (model != nullptr) {
        header_texts_ = connect(model, &QAbstractItemModel::headerDataChanged, this, &GridTableView::fit_header_height);
    }
    fit_header_height();
}

void GridTableView::fit_header_height()
{
    QHeaderView* columns = horizontalHeader();
    if (model() == nullptr) {
        return;
    }
    // The tallest wrapped header text beyond one line, measured in the style's label rect.
    const QFontMetrics metrics = columns->fontMetrics();
    int extra = 0;
    for (int section = 0; section < columns->count(); ++section) {
        if (columns->isSectionHidden(section)) {
            continue;
        }
        QStyleOptionHeader option;
        option.initFrom(columns);
        option.orientation = Qt::Horizontal;
        option.section = section;
        option.rect = QRect{0, 0, columns->sectionSize(section), metrics.height() * 4};
        option.text = model()->headerData(section, Qt::Horizontal).toString();
        option.textAlignment = columns->defaultAlignment();
        if (columns->sortIndicatorSection() == section) {
            option.sortIndicator = columns->sortIndicatorOrder() == Qt::AscendingOrder ? QStyleOptionHeader::SortDown
                                                                                       : QStyleOptionHeader::SortUp;
        }
        const QRect label = columns->style()->subElementRect(QStyle::SE_HeaderLabel, &option, columns);
        const QRect text = metrics.boundingRect(QRect{0, 0, std::max(1, label.width()), metrics.height() * 16},
                                                Qt::AlignLeft | Qt::TextWordWrap, option.text);
        extra = std::max(extra, text.height() - metrics.height());
    }
    const int minimum = extra > 0 ? columns->sizeHint().height() + extra : 0;
    if (columns->minimumHeight() != minimum) {
        columns->setMinimumHeight(minimum);
        updateGeometries();
    }
}

void GridTableView::set_fill_column(int logical_column, int minimum_width)
{
    fill_column_ = logical_column;
    fill_minimum_ = minimum_width;
    fit_fill_column();
}

void GridTableView::fit_fill_column()
{
    QHeaderView* columns = horizontalHeader();
    if (fitting_ || fill_column_ < 0 || fill_column_ >= columns->count() || columns->isSectionHidden(fill_column_)) {
        return;
    }
    int others = 0;
    for (int section = 0; section < columns->count(); ++section) {
        if (section != fill_column_ && !columns->isSectionHidden(section)) {
            others += columns->sectionSize(section);
        }
    }
    const int width = std::max(fill_minimum_, viewport()->width() - others);
    if (columns->sectionSize(fill_column_) != width) {
        fitting_ = true;
        columns->resizeSection(fill_column_, width);
        fitting_ = false;
    }
}

int GridTableView::row_height() const
{
    return fontMetrics().height() + 12;  // comfortable rows (D-53)
}

void GridTableView::show_current_row(std::optional<int> row, ui::ScrollRequest request, int column,
                                     bool keep_horizontal)
{
    QItemSelectionModel* selection = selectionModel();
    if (!row) {
        if (selection->currentIndex().isValid() || selection->hasSelection()) {
            selection->clear();
        }
        return;
    }
    if (request != ui::ScrollRequest::none) {
        doItemsLayout();  // fresh scroll ranges after a model reset, before scrolling
    }
    const QModelIndex current = selection->currentIndex();
    const QModelIndex target =
        model()->index(*row, request == ui::ScrollRequest::none && current.isValid() ? current.column() : column);
    if (current != target || !selection->isRowSelected(target.row())) {
        const bool auto_scroll = hasAutoScroll();
        setAutoScroll(false);  // the scroll position follows the request below
        selection->setCurrentIndex(target, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        setAutoScroll(auto_scroll);
    }
    switch (request) {
    case ui::ScrollRequest::top:
    case ui::ScrollRequest::top_then_current:
        scrollToTop();  // the full reload shows row 0 first (01 §8.6) ...
        if (!keep_horizontal) {
            // ... then the current cell (row 0 or the restored pick) is scrolled into view, horizontally
            // too, as setting DataGridView.CurrentCell does (01 §8.7, §9.3).
            scrollTo(target);
        }
        break;
    case ui::ScrollRequest::current:
        scrollTo(target);
        break;
    case ui::ScrollRequest::none:
        break;  // keep the first displayed row (streaming in All fares)
    }
}

void GridTableView::show_sort_indicator(int section, Qt::SortOrder order)
{
    QHeaderView* columns = horizontalHeader();
    if (columns->sortIndicatorSection() != section || columns->sortIndicatorOrder() != order) {
        columns->setSortIndicator(section, order);
    }
}

bool GridTableView::event(QEvent* event)
{
    // Claim Enter before any window shortcut (the AcceptButton emulation) can take it.
    if (event->type() == QEvent::ShortcutOverride && is_enter(*static_cast<QKeyEvent*>(event))) {
        event->accept();
        return true;
    }
    return QTableView::event(event);
}

void GridTableView::changeEvent(QEvent* event)
{
    QTableView::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        apply_row_height();
        fit_header_height();
    }
}

void GridTableView::contextMenuEvent(QContextMenuEvent* event)
{
    QPoint global_pos = event->globalPos();
    if (event->reason() == QContextMenuEvent::Keyboard) {
        const QRect current = visualRect(currentIndex());
        const QPoint anchor = current.isValid() && viewport()->rect().intersects(current) ? current.bottomLeft() : QPoint{};
        global_pos = viewport()->mapToGlobal(anchor);
    }
    event->accept();
    Q_EMIT context_menu_requested(global_pos);
}

void GridTableView::focusInEvent(QFocusEvent* event)
{
    QTableView::focusInEvent(event);
    viewport()->update();  // the accent bar of the selected row, or the ring of an empty table
}

void GridTableView::focusOutEvent(QFocusEvent* event)
{
    QTableView::focusOutEvent(event);
    viewport()->update();
}

void GridTableView::paintEvent(QPaintEvent* event)
{
    QTableView::paintEvent(event);
    if (hasFocus() && !currentIndex().isValid()) {
        // No row can carry the accent bar: ring the table instead, so focus stays visible.
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(style::theme().accent_line, 2.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(viewport()->rect()).adjusted(1, 1, -1, -1), 3, 3);
    }
}

void GridTableView::keyPressEvent(QKeyEvent* event)
{
    if (is_enter(*event)) {
        event->accept();  // handled: no move to the next row, no default button (01 §8.8)
        Q_EMIT enter_pressed();
        return;
    }
    QTableView::keyPressEvent(event);
}

void GridTableView::keyboardSearch(const QString& /*search*/)
{
    // No type-ahead: a read-only DataGridView ignores typed characters (01 §8.8).
}

void GridTableView::keyReleaseEvent(QKeyEvent* event)
{
    if (is_navigation_key(event->key()) && !event->isAutoRepeat()) {
        Q_EMIT navigation_key_released();
    }
    QTableView::keyReleaseEvent(event);
}

void GridTableView::mouseDoubleClickEvent(QMouseEvent* event)
{
    const QPersistentModelIndex index{indexAt(event->position().toPoint())};
    QTableView::mouseDoubleClickEvent(event);
    if (event->button() == Qt::LeftButton && index.isValid()) {
        Q_EMIT data_cell_double_clicked(index.row());
    }
}

void GridTableView::mousePressEvent(QMouseEvent* event)
{
    const QModelIndex index = indexAt(event->position().toPoint());
    if (!index.isValid()) {
        // Empty space: nothing changes; a right-click menu then acts on the current row.
        setFocus(Qt::MouseFocusReason);
        event->accept();
        return;
    }
    Q_EMIT data_cell_pressed(index.row(), event->button());
    if (event->button() == Qt::MiddleButton) {
        setFocus(Qt::MouseFocusReason);  // the middle button picks but does not move the current row
        event->accept();
        return;
    }
    QTableView::mousePressEvent(event);  // left and right buttons make the cell current
}

void GridTableView::resizeEvent(QResizeEvent* event)
{
    QTableView::resizeEvent(event);
    fit_fill_column();
}

void GridTableView::apply_theme()
{
    QPalette colours = palette();
    colours.setColor(QPalette::Base, style::theme().surface);
    colours.setColor(QPalette::Text, style::theme().text);
    setPalette(colours);
}

void GridTableView::apply_row_height()
{
    QHeaderView* rows = verticalHeader();
    const int height = row_height();
    rows->setMinimumSectionSize(std::min(rows->minimumSectionSize(), height));
    rows->setDefaultSectionSize(height);
}

} // namespace wizz::app
