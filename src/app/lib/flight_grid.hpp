#pragma once
// FlightGrid: the Outbound and Return tabs (01 §8). A top bar ("Show:", the
// view combo, the grey summary) over a GridTableView bound to a
// FlightTableModel. Every rule lives in wizz::ui::FlightGridModel; this widget
// forwards user input to it and mirrors its current row and scroll requests
// into the view:
//
//   * Clear / Append / SetResults / SetSummary (01 §8.6) with the automatic
//     view switch unless the user ever chose a view (LIVE-5);
//   * the current row (`Selected`) versus the explicit pick (01 §8.7, U-09):
//     mouse down on a data cell (any button) and the key-up of a navigation
//     key set the pick; only Clear resets it;
//   * double-click / Enter open the booking URL, right-click makes the row
//     current and opens the context menu, header clicks sort (01 §8.4, §8.8,
//     §8.9); Copy details with the clipboard retry and its busy box;
//   * the self-test accessors of 01 §8.10.
//
// UI thread only.

#include "flight_table_model.hpp"
#include "grid_actions.hpp"

#include "wizz/ui/flight_rows.hpp"

#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"

#include <QPoint>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class QAction;
class QComboBox;
class QMenu;

namespace wizz::app {

class ElidedLabel;
class GridTableView;

class FlightGrid final : public QWidget {
    Q_OBJECT

public:
    /// `offer_plan_return`: true for the Outbound grid (the third context-menu item).
    explicit FlightGrid(std::shared_ptr<const Locale> locale, bool offer_plan_return, QWidget* parent = nullptr);
    ~FlightGrid() override;

    // --- Data flow (01 §8.6) ------------------------------------------------------------

    /// Start of a leg: no fares, currency upper-cased, the pick reset, the view switched to
    /// "Cheapest per destination" (anywhere) or "All fares" unless the user chose a view,
    /// headers updated, the summary shown.
    void clear(std::string_view currency, bool anywhere, std::string_view summary = {});
    /// A streamed batch (the 150 ms flush). All fares: rows go to their sorted positions, the
    /// current row stays on its fare and the scroll position is kept; grouped views rebuild.
    void append(std::span<const FlightResultPtr> batch);
    /// The final or re-priced result: replaces every fare; keeps anywhere, view, sort and pick.
    void set_results(std::span<const FlightResultPtr> results, std::string_view currency, std::string_view summary);
    void set_summary(std::string_view summary);

    // --- State (01 §8.7, §8.10) ---------------------------------------------------------

    /// `Selected`: the current row's fare; non-null whenever there are rows.
    [[nodiscard]] FlightResultPtr selected() const;
    /// `ExplicitSelection`: the fare the user picked since the last Clear, or null.
    [[nodiscard]] FlightResultPtr explicit_selection() const;
    /// `AllResults`: every fare the grid holds (the tab title counts these, 01 §5).
    [[nodiscard]] std::span<const FlightResultPtr> all_results() const;
    /// `ShownResults`: the current view list.
    [[nodiscard]] std::span<const FlightResultPtr> shown_results() const;
    [[nodiscard]] ui::ViewMode mode() const;
    /// `RowCount`: rows of the current view.
    [[nodiscard]] std::size_t row_count() const;
    [[nodiscard]] const std::string& summary_text() const;
    [[nodiscard]] bool offers_plan_return() const noexcept { return plan_return_ != nullptr; }
    /// `VisibleRowsText(max)`: the first `max` rows in display order (01 §8.10).
    [[nodiscard]] std::vector<std::string> visible_rows_text(std::size_t max) const;
    /// `CurrentRowText` (01 §8.10).
    [[nodiscard]] std::string current_row_text() const;

    // --- Environment ----------------------------------------------------------------------

    /// `IsBusy()` for the context menu's third item (UI-9); not busy when unset.
    void set_busy_query(std::function<bool()> is_busy);
    /// Browser, clipboard and MessageBox side effects (GridActions::system() by default).
    void set_actions(GridActions actions);

    // --- Actions (also bound to the mouse, the keyboard and the context menu) ---------------

    /// `OpenSelected()`: opens `Selected.BookingUrl`; a failure shows the browser Warning box.
    void open_selected();
    /// `CopySelected()`: the "Copy details" text on the clipboard, retried about 10 × 100 ms,
    /// then the Information box "The clipboard is busy …".
    void copy_selected();
    /// "Search return flights from this destination": raises plan_return_requested(Selected).
    void request_plan_return();
    /// The context menu with its texts and enabled states set for the current row and
    /// IsBusy() (01 §8.9); it is shown by right-clicks, the Menu key and Shift+F10.
    QMenu* prepare_context_menu();

    // --- Parts (MainWindow focus rules, autotest, tests) ------------------------------------

    [[nodiscard]] FlightTableModel* model() const noexcept { return model_; }
    [[nodiscard]] GridTableView* table() const noexcept { return table_; }
    [[nodiscard]] QComboBox* view_combo() const noexcept { return view_; }
    [[nodiscard]] ElidedLabel* summary_label() const noexcept { return summary_; }
    [[nodiscard]] QMenu* context_menu() const noexcept { return menu_; }

Q_SIGNALS:
    /// The user asked for return flights from `fare`'s destination (01 §8.9, §11.6).
    void plan_return_requested(wizz::FlightResultPtr fare);
    /// Opening a booking URL failed (after the Warning box was shown).
    void open_url_failed(const QString& url, const QString& message);
    /// After every change of the fares (Clear, Append, SetResults): tab titles (01 §5).
    void contents_changed();

private:
    void build_columns();
    void on_view_activated(int index);
    void on_header_clicked(int section);
    void on_cell_pressed(int row, Qt::MouseButton button);
    void on_current_row_changed(const QModelIndex& current);
    void show_context_menu(QPoint global_pos);
    void try_copy();
    /// UpdateHeaders, the combo, the summary label, then the current row and scroll.
    void after_change();
    void update_headers();
    void update_sort_indicator();
    void sync_current_row();

    FlightTableModel* model_ = nullptr;
    GridTableView* table_ = nullptr;
    QComboBox* view_ = nullptr;
    ElidedLabel* summary_ = nullptr;
    QMenu* menu_ = nullptr;
    QAction* open_ = nullptr;
    QAction* copy_ = nullptr;
    QAction* plan_return_ = nullptr;
    QTimer clipboard_retry_;
    QString pending_copy_;
    int copy_attempts_left_ = 0;
    std::function<bool()> is_busy_;
    GridActions actions_;
    bool syncing_ = false;  ///< model-driven view changes are not user navigation
};

} // namespace wizz::app
