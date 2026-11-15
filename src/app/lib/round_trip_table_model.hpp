#pragma once
// Qt table model of the Round trips tab (01 §9.2-§9.4) over
// wizz::ui::RoundTripGridModel, which owns the rows, the default sort
// (Total, Out date, then the combiner order), header sorting and the current
// trip. SetTrips is one model reset; a header sort is one layout change with
// the persistent indexes moved to their rows' new positions. No per-row
// colouring. UI thread only.

#include "keyed_table_model.hpp"

#include "wizz/ui/round_trip_rows.hpp"

#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"

#include <QFont>

#include <memory>
#include <string_view>
#include <vector>

namespace wizz::app {

class RoundTripTableModel final : public KeyedTableModel {
    Q_OBJECT

public:
    explicit RoundTripTableModel(std::shared_ptr<const Locale> locale, QObject* parent = nullptr);

    /// Model column (section) of a data column; the order of 01 §9.2, Idx included.
    [[nodiscard]] static int column_index(ui::TripColumn column) noexcept;
    /// Data column of a model column; `section` must be in [0, 10).
    [[nodiscard]] static ui::TripColumn column_at(int section) noexcept;
    /// Cell alignment (01 §9.2): Nights centred; Total and the two prices right.
    [[nodiscard]] static Qt::Alignment alignment(ui::TripColumn column) noexcept;

    // --- QAbstractTableModel ----------------------------------------------------------

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

    // --- Operations ---------------------------------------------------------------------

    /// `SetTrips` (01 §9.3): the rows replaced, row 0 current and scrolled to the top.
    void set_trips(std::vector<RoundTrip> trips, std::string_view currency, std::string_view summary);
    /// A header click: that column ascending first, then toggled; persists across SetTrips.
    void sort_by_column(ui::TripColumn column);
    /// Mouse down on data row `row` (display order): left and right buttons make it current.
    void mouse_down(int row, ui::MouseButton button);
    /// The view's current row moved.
    void set_current_row(int row);
    /// The filter checkboxes; true when the value changed.
    bool set_require_same_home(bool value) noexcept { return grid_.set_require_same_home(value); }
    bool set_require_same_away(bool value) noexcept { return grid_.set_require_same_away(value); }

    [[nodiscard]] const ui::RoundTripGridModel& state() const noexcept { return grid_; }
    [[nodiscard]] ui::ScrollRequest take_scroll_request() noexcept { return grid_.take_scroll_request(); }

private:
    ui::RoundTripGridModel grid_;
    QFont bold_;
};

} // namespace wizz::app
