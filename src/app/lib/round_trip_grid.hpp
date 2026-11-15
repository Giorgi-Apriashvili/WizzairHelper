#pragma once
// RoundTripGrid: the Round trips tab (01 §9). A top bar with the two filter
// checkboxes (not persisted, enabled during searches) and the grey summary,
// over a GridTableView bound to a RoundTripTableModel. MainWindow computes the
// trips (ui::recompute_trips, 01 §11.8) and hands them to set_trips(); a
// checkbox change raises filters_changed() so it can recompute. Double-click /
// Enter open the combined booking or the outbound then the return booking;
// right-click makes the row current and opens the menu "Open on wizzair.com",
// "Open outbound only", "Open return only" (always enabled; no-ops without a
// current trip). UI thread only.

#include "grid_actions.hpp"
#include "round_trip_table_model.hpp"

#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"

#include <QPoint>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class QAction;
class QCheckBox;
class QLabel;
class QMenu;

namespace wizz::app {

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

    // --- Actions (01 §9.4) ------------------------------------------------------------------

    /// `OpenSelected()`: the combined booking URL, else the outbound URL then the return URL.
    void open_selected();
    /// "Open outbound only".
    void open_outbound();
    /// "Open return only".
    void open_return();
    /// The context menu (its items are always enabled).
    [[nodiscard]] QMenu* context_menu() const noexcept { return menu_; }

    // --- Parts (tests, autotest screenshots) ------------------------------------------------

    [[nodiscard]] RoundTripTableModel* model() const noexcept { return model_; }
    [[nodiscard]] GridTableView* table() const noexcept { return table_; }
    [[nodiscard]] QCheckBox* same_home_box() const noexcept { return same_home_; }
    [[nodiscard]] QCheckBox* same_away_box() const noexcept { return same_away_; }
    [[nodiscard]] QLabel* summary_label() const noexcept { return summary_; }

Q_SIGNALS:
    /// Either checkbox changed (`FiltersChanged` → MainForm.RecomputeTrips).
    void filters_changed();
    /// Opening a booking URL failed (after the Warning box was shown).
    void open_url_failed(const QString& url, const QString& message);

private:
    void open_urls(const std::vector<std::string>& urls);
    void on_header_clicked(int section);
    void on_cell_pressed(int row, Qt::MouseButton button);
    void update_sort_indicator();
    void sync_current_row();

    RoundTripTableModel* model_ = nullptr;
    GridTableView* table_ = nullptr;
    QCheckBox* same_home_ = nullptr;
    QCheckBox* same_away_ = nullptr;
    QLabel* summary_ = nullptr;
    QMenu* menu_ = nullptr;
    GridActions actions_;
    bool syncing_ = false;  ///< model-driven view changes are not user navigation
};

} // namespace wizz::app
