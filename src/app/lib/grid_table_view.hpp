#pragma once
// The table of both result grids (01 §8.1, §9.1): read-only, full-row selection of
// several rows (DECISIONS D-76: a click, Ctrl+click, Shift+click, drag, Shift+arrows,
// Ctrl+Space, Ctrl+A, as Windows lists select), no row headers, no border, fixed row height, columns resizable but
// not movable, header clicks reported (the grids sort through ui_logic, not
// through QTableView's own sorting), the last data column filling the
// remaining width (WinForms AutoSizeMode.Fill with a minimum width), header
// texts that wrap with an auto-sized header height (DataGridView headers:
// WrapMode True, ColumnHeadersHeightSizeMode AutoSize) and GridRowDelegate
// cell painting. Its surface and text colours follow the look (ui_style.hpp);
// keyboard focus shows as the accent bar of the current row (selected or not),
// or as an accent ring round a table without a current row.
//
// It turns raw input into intent signals so the grid widgets stay free of
// event plumbing: a press on a data cell (before Qt moves the current cell),
// a left double-click on a data cell, Enter (consumed here so it never reaches
// a window default button, 01 §6.3 / §8.8), Ctrl+C (the grid's own copy, not
// Qt's copy of one cell, D-76), the key-up of a navigation key
// (the explicit pick, 01 §8.7) and context-menu requests from the mouse, the
// Menu key or Shift+F10.

#include "wizz/ui/flight_rows.hpp"

#include <QItemSelectionModel>
#include <QPoint>
#include <QTableView>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

class QContextMenuEvent;
class QEvent;
class QFocusEvent;
class QPaintEvent;
class QKeyEvent;
class QMouseEvent;
class QResizeEvent;

namespace wizz::app {

class GridTableView final : public QTableView {
    Q_OBJECT

public:
    explicit GridTableView(QWidget* parent = nullptr);

    /// Makes `logical_column` fill the width left by the other visible columns, but never
    /// narrower than `minimum_width` (then the table scrolls horizontally).
    void set_fill_column(int logical_column, int minimum_width);
    /// Re-applies the fill rule (after a column was shown or hidden).
    void fit_fill_column();
    /// Row height: font height + 12 (DECISIONS D-53; WinForms used + 9).
    [[nodiscard]] int row_height() const;
    /// Header height: one text line, or as many as the longest wrapped header text needs.
    void fit_header_height();

    void setModel(QAbstractItemModel* model) override;

    /// Mirrors the grid model's current row and selection into the view: `row` (display order)
    /// becomes the current row — at `column`, or at the current column when nothing scrolls —,
    /// exactly the rows of `selected` (display order, ascending; D-76) are selected, and the
    /// scroll request is applied: `top` and `top_then_current` show row 0 first, then scroll the
    /// current cell into view (horizontally too) unless `keep_horizontal` asks only for the top
    /// (a reload without RestoreSelection); `current` scrolls the current cell into view; `none`
    /// keeps the first displayed row. No row clears the selection.
    void show_current_row(std::optional<int> row, std::span<const std::size_t> selected, ui::ScrollRequest request,
                          int column, bool keep_horizontal = false);
    /// The selected rows in display order, ascending.
    [[nodiscard]] std::vector<std::size_t> selected_rows() const;

    /// The header glyph of the user's sort; `section` -1 shows none (the default sort).
    void show_sort_indicator(int section, Qt::SortOrder order);
    /// Does nothing: typed characters never move the current row (no type-ahead, 01 §8.8).
    void keyboardSearch(const QString& search) override;

Q_SIGNALS:
    /// Mouse button pressed on a data cell (`row` in display order), before the view
    /// changes its current cell. Presses on empty space or the header are not reported.
    void data_cell_pressed(int row, Qt::MouseButton button);
    /// Left double-click on a data cell.
    void data_cell_double_clicked(int row);
    /// Enter or Return without modifiers; the key never propagates further.
    void enter_pressed();
    /// Ctrl+C (QKeySequence::Copy); the key never propagates further (D-76).
    void copy_requested();
    /// Key-up of Up, Down, PageUp, PageDown, Home or End.
    void navigation_key_released();
    /// Right-click anywhere in the table (also on empty space or the header), the Menu key
    /// or Shift+F10; `global_pos` is where the menu should open.
    void context_menu_requested(QPoint global_pos);

protected:
    /// Qt's ExtendedSelection commands, except that a plain left press on a selected row selects it
    /// alone at once instead of at the release (D-76).
    [[nodiscard]] QItemSelectionModel::SelectionFlags selectionCommand(const QModelIndex& index,
                                                                        const QEvent* event = nullptr) const override;
    /// Repaints the whole previous and new current rows: the accent bar of the current row
    /// is drawn at the view's left edge, which need not be the current cell.
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void apply_row_height();
    void apply_theme();

    int fill_column_ = -1;
    int fill_minimum_ = 0;
    bool fitting_ = false;
    QMetaObject::Connection header_texts_;
};

} // namespace wizz::app
