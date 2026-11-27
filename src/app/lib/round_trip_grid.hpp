#pragma once
// RoundTripGrid: the Round trips tab (01 §9). A top bar with the two filter
// checkboxes (not persisted, enabled during searches) and the grey summary,
// over a GridTableView bound to a RoundTripTableModel. MainWindow computes the
// trips (ui::recompute_trips, 01 §11.8) and hands them to set_trips(); a
// checkbox change raises filters_changed() so it can recompute. Double-click /
// Enter open the combined booking or the outbound then the return booking;
// right-click makes the row current and opens the menu "Open on wizzair.com",
// "Open outbound only", "Open return only" (always enabled; no-ops without a
// current trip), after a separator "Copy details" (also Ctrl+C) and "Copy as
// table" of the selected trips (D-76; enabled while there are trips), and after
// another "Homes on Airbnb" (D-72), which like the button at the right end of the
// top bar is enabled while there is a current trip and raises homes_requested with
// its stay. Several trips can be selected (D-76); they stay selected through a
// sort until the next set_trips; the open items and Homes act on the current
// trip. UI thread only.

#include "clipboard_copy.hpp"
#include "grid_actions.hpp"
#include "round_trip_table_model.hpp"

#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"
#include "wizz/ui/homes_stay.hpp"

#include <QPoint>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class QAction;
class QCheckBox;
class QMenu;

namespace wizz::app {

class ElidedLabel;
class GridBarButton;
class GridTableView;

class RoundTripGrid final : public QWidget {
    Q_OBJECT

public:
    explicit RoundTripGrid(std::shared_ptr<const Locale> locale, QWidget* parent = nullptr);
    ~RoundTripGrid() override;

    /// `SetTrips(trips, currency, summary)` (01 §9.3): rows replaced, currency upper-cased,
    /// row 0 current (Total column) and scrolled to the top, the summary shown. The
    /// selection always goes back to the top on a recompute.
    void set_trips(std::vector<RoundTrip> trips, std::string_view currency, std::string_view summary);

    // --- State (01 §9.5) ------------------------------------------------------------------

    /// `Count`: the trips shown (at most 500; the tab title counts these).
    [[nodiscard]] std::size_t count() const;
    [[nodiscard]] std::span<const RoundTrip> trips() const;
    [[nodiscard]] const std::string& summary_text() const;
    /// `RequireSameHomeAirport` = "Return to the airport I left from" is checked.
    [[nodiscard]] bool require_same_home() const;
    /// `RequireSameAwayAirport` = "Fly back from the airport I landed at" is checked.
    [[nodiscard]] bool require_same_away() const;
    /// The current trip, or null.
    [[nodiscard]] const RoundTrip* selected() const;

    /// Browser and MessageBox side effects (GridActions::system() by default).
    void set_actions(GridActions actions);
    [[nodiscard]] const GridActions& actions() const noexcept { return actions_; }
    /// Today's local date for "Homes on Airbnb" (a trip that has begun has no stay, D-72);
    /// while unset, every trip has one.
    void set_today_query(std::function<Date()> today);

    // --- Actions (01 §9.4) ------------------------------------------------------------------

    /// `OpenSelected()`: the combined booking URL, else the outbound URL then the return URL.
    void open_selected();
    /// "Open outbound only".
    void open_outbound();
    /// "Open return only".
    void open_return();
    /// "Copy details" (also Ctrl+C, D-76): both legs' details and the total of every selected
    /// trip on the clipboard, retried like the flight grids' copy.
    void copy_selected();
    /// "Copy as table" (D-76): the selected trips as a tab-separated table.
    void copy_table();
    /// The current trip's stay ("Homes on Airbnb", ui::stay_for_trip), or nullopt.
    [[nodiscard]] std::optional<ui::HomesStay> selected_stay() const;
    /// "Homes on Airbnb": raises homes_requested with the current trip's stay, if any.
    void request_homes();
    /// The context menu with "Homes on Airbnb" enabled for the current trip and the two copy
    /// items while there are trips (the three booking items are always enabled).
    QMenu* prepare_context_menu();
    [[nodiscard]] QMenu* context_menu() const noexcept { return menu_; }

    // --- Parts (tests, autotest screenshots) ------------------------------------------------

    [[nodiscard]] RoundTripTableModel* model() const noexcept { return model_; }
    [[nodiscard]] GridTableView* table() const noexcept { return table_; }
    [[nodiscard]] QCheckBox* same_home_box() const noexcept { return same_home_; }
    [[nodiscard]] QCheckBox* same_away_box() const noexcept { return same_away_; }
    [[nodiscard]] ElidedLabel* summary_label() const noexcept { return summary_; }
    [[nodiscard]] GridBarButton* homes_button() const noexcept { return homes_; }

Q_SIGNALS:
    /// Either checkbox changed (`FiltersChanged` → MainForm.RecomputeTrips).
    void filters_changed();
    /// Opening a booking URL failed (after the Warning box was shown).
    void open_url_failed(const QString& url, const QString& message);
    /// The user asked for the homes on Airbnb around `stay`'s destination (D-72).
    void homes_requested(const wizz::ui::HomesStay& stay);

private:
    void open_urls(const std::vector<std::string>& urls);
    void on_header_clicked(int section);
    void on_cell_pressed(int row, Qt::MouseButton button);
    void on_selection_changed();
    void update_sort_indicator();
    void sync_current_row();
    /// The button follows whether there is a current trip.
    void refresh_homes();

    RoundTripTableModel* model_ = nullptr;
    GridTableView* table_ = nullptr;
    QCheckBox* same_home_ = nullptr;
    QCheckBox* same_away_ = nullptr;
    ElidedLabel* summary_ = nullptr;
    GridBarButton* homes_ = nullptr;
    QMenu* menu_ = nullptr;
    QAction* copy_action_ = nullptr;
    QAction* copy_table_action_ = nullptr;
    QAction* homes_action_ = nullptr;
    std::function<Date()> today_;
    GridActions actions_;
    ClipboardCopy* clipboard_ = nullptr;
    bool syncing_ = false;  ///< model-driven view changes are not user navigation
};

} // namespace wizz::app
