#pragma once
// Qt table model of the Outbound and Return grids (01 §8.2-§8.7, 06 §2) over
// wizz::ui::FlightGridModel, which owns every rule: the index vector over the
// current view list, the default and user sorts, the cheapest row, view
// switching, the explicit pick and RestoreSelection. This class only maps that
// state to Qt roles and raises the right model notifications:
//
//   * full rebuilds (Clear, SetResults, a view change, a flush in a grouped
//     view) are one beginResetModel/endResetModel;
//   * a streamed batch in "All fares" puts each new row at its sorted position
//     (not a contiguous block), so it is one layoutAboutToBeChanged/layoutChanged
//     pair with the persistent indexes moved to their rows' new positions, the
//     way QSortFilterProxyModel re-sorts; a batch into an empty grid is one
//     beginInsertRows/endInsertRows;
//   * a header sort is one layout change.
//
// Cells are formatted on demand for the visible rows only. UI thread only.

#include "keyed_table_model.hpp"

#include "wizz/ui/flight_rows.hpp"

#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"

#include <QBrush>
#include <QFont>

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace wizz::app {

class FlightTableModel final : public KeyedTableModel {
    Q_OBJECT

public:
    /// Roles beyond Display, TextAlignment, Font, Foreground, Background and
    /// GridRowDelegate::SelectionBackgroundRole (the cheapest rows' selection colour).
    enum Role {
        /// int(ui::RowTone) of the row (01 §8.3).
        RowToneRole = Qt::UserRole + 1,
    };

    /// `offer_plan_return`: the Outbound grid offers "Search return flights from this destination".
    explicit FlightTableModel(std::shared_ptr<const Locale> locale, bool offer_plan_return = true,
                              QObject* parent = nullptr);

    /// Model column (section) of a data column; the order of 01 §8.2, Idx and PriceSort included.
    [[nodiscard]] static int column_index(ui::FlightColumn column) noexcept;
    /// Data column of a model column; `section` must be in [0, 13).
    [[nodiscard]] static ui::FlightColumn column_at(int section) noexcept;
    /// Cell alignment (01 §8.2): Departs centred; km, Total, Per person and Fare (local) right.
    [[nodiscard]] static Qt::Alignment alignment(ui::FlightColumn column) noexcept;

    // --- QAbstractTableModel ----------------------------------------------------------

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;

    // --- Data flow (01 §8.6) ------------------------------------------------------------

    /// Start of a leg: no fares, the pick reset, the automatic view switch (LIVE-5).
    void clear(std::string_view currency, bool anywhere, std::string_view summary = {});
    /// A streamed batch (the 150 ms flush).
    void append(std::span<const FlightResultPtr> batch);
    /// The final or re-priced result; keeps anywhere, the view, the sort and the pick.
    void set_results(std::span<const FlightResultPtr> results, std::string_view currency, std::string_view summary);
    void set_summary(std::string_view summary);

    // --- User actions -------------------------------------------------------------------

    /// A user change of the "Show:" combo (marks the view as chosen); no-op for the current mode.
    void choose_view(ui::ViewMode mode);
    /// A header click: that column ascending first, then toggled; the current fare stays current.
    void sort_by_column(ui::FlightColumn column);
    /// Mouse down on data row `row` (display order): the explicit pick, and for the left and
    /// right buttons the current row.
    void mouse_down(int row, ui::MouseButton button);
    /// The view's current row moved (keyboard, click); the pick is unchanged.
    void set_current_row(int row);
    /// Key-up of a navigation key: the current fare becomes the explicit pick.
    void navigation_key_up();

    // --- State --------------------------------------------------------------------------

    /// Everything the grid knows (mode, pick, current row, cheapest, summary, sort, …).
    [[nodiscard]] const ui::FlightGridModel& state() const noexcept { return grid_; }
    /// The fare of display row `row`, or null.
    [[nodiscard]] FlightResultPtr fare_at(int row) const;
    /// The view's scroll duty after the last change (taken once).
    [[nodiscard]] ui::ScrollRequest take_scroll_request() noexcept { return grid_.take_scroll_request(); }
    /// True once after a flush into a grouped view without an explicit pick: that rebuild runs
    /// no RestoreSelection (01 §8.6), so it returns to the top without making a cell current
    /// through CurrentCell, and the horizontal scroll position stays.
    [[nodiscard]] bool take_keep_horizontal_scroll() noexcept { return std::exchange(keep_horizontal_scroll_, false); }

private:
    /// The `Idx` of display row `row` (the key that persistent indexes follow).
    [[nodiscard]] std::size_t idx_at(int row) const;
    void notify_currency(const std::string& before);

    ui::FlightGridModel grid_;
    QFont bold_;
    QBrush gray_text_;
    bool keep_horizontal_scroll_ = false;
};

} // namespace wizz::app
