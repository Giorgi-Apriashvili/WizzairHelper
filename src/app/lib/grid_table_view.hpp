#pragma once
// The table of both result grids (01 §8.1, §9.1): read-only, full-row single
// selection, no row headers, no border, fixed row height, columns resizable but
// not movable, header clicks reported (the grids sort through ui_logic, not
// through QTableView's own sorting), the last data column filling the
// remaining width (WinForms AutoSizeMode.Fill with a minimum width), header
// texts that wrap with an auto-sized header height (DataGridView headers:
// WrapMode True, ColumnHeadersHeightSizeMode AutoSize) and GridRowDelegate
// cell painting.
//
// It turns raw input into intent signals so the grid widgets stay free of
// event plumbing: a press on a data cell (before Qt moves the current cell),
// a left double-click on a data cell, Enter (consumed here so it never reaches
// a window default button, 01 §6.3 / §8.8), the key-up of a navigation key
// (the explicit pick, 01 §8.7) and context-menu requests from the mouse, the
// Menu key or Shift+F10.

#include "wizz/ui/flight_rows.hpp"

#include <QPoint>
#include <QTableView>

#include <optional>

class QContextMenuEvent;
class QEvent;
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
    /// Row height: font height + 9, the WinForms DataGridView row template.
    [[nodiscard]] int row_height() const;
    /// Header height: one text line, or as many as the longest wrapped header text needs.
    void fit_header_height();

    void setModel(QAbstractItemModel* model) override;

    /// Mirrors the grid model's current row into the view: `row` (display order) becomes the
    /// current, selected row — at `column`, or at the current column when nothing scrolls —
    /// and the scroll request is applied: `top` shows row 0 first, `current` scrolls the row
    /// into view, `none` keeps the first displayed row. No row clears the selection.
    void show_current_row(std::optional<int> row, ui::ScrollRequest request, int column);
    /// The header glyph of the user's sort; `section` -1 shows none (the default sort).
    void show_sort_indicator(int section, Qt::SortOrder order);

Q_SIGNALS:
    /// Mouse button pressed on a data cell (`row` in display order), before the view
    /// changes its current cell. Presses on empty space or the header are not reported.
    void data_cell_pressed(int row, Qt::MouseButton button);
    /// Left double-click on a data cell.
    void data_cell_double_clicked(int row);
    /// Enter or Return without modifiers; the key never propagates further.
    void enter_pressed();
    /// Key-up of Up, Down, PageUp, PageDown, Home or End.
    void navigation_key_released();
    /// Right-click anywhere in the table (also on empty space or the header), the Menu key
    /// or Shift+F10; `global_pos` is where the menu should open.
    void context_menu_requested(QPoint global_pos);

protected:
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void apply_row_height();

    int fill_column_ = -1;
    int fill_minimum_ = 0;
    bool fitting_ = false;
    QMetaObject::Connection header_texts_;
};

} // namespace wizz::app
