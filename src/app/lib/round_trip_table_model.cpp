#include "round_trip_table_model.hpp"

#include <cstddef>
#include <utility>

namespace wizz::app {

namespace {

// Model columns are the data columns of 01 §9.2 in order, so the enum value is the section.
static_assert([] {
    for (std::size_t i = 0; i < ui::kTripColumnCount; ++i) {
        if (ui::kTripColumns[i] != static_cast<ui::TripColumn>(i)) {
            return false;
        }
    }
    return true;
}());

[[nodiscard]] constexpr std::size_t to_size(int value) noexcept
{
    return static_cast<std::size_t>(value);
}

} // namespace

RoundTripTableModel::RoundTripTableModel(std::shared_ptr<const Locale> locale, QObject* parent)
    : KeyedTableModel(parent), grid_(std::move(locale))
{
    bold_.setBold(true);  // only the weight is set: it resolves against the view's font
}

int RoundTripTableModel::column_index(ui::TripColumn column) noexcept
{
    return static_cast<int>(column);
}

ui::TripColumn RoundTripTableModel::column_at(int section) noexcept
{
    return ui::kTripColumns[to_size(section)];
}

Qt::Alignment RoundTripTableModel::alignment(ui::TripColumn column) noexcept
{
    switch (column) {
    case ui::TripColumn::nights:
        return Qt::AlignHCenter | Qt::AlignVCenter;
    case ui::TripColumn::total:
    case ui::TripColumn::out_price:
    case ui::TripColumn::return_price:
        return Qt::AlignRight | Qt::AlignVCenter;
    default:
        return Qt::AlignLeft | Qt::AlignVCenter;
    }
}

int RoundTripTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(grid_.row_count());
}

int RoundTripTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(ui::kTripColumnCount);
}

QVariant RoundTripTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount()) {
        return {};
    }
    const ui::TripColumn column = column_at(index.column());
    switch (role) {
    case Qt::DisplayRole:
        return QString::fromStdString(grid_.cell_text(to_size(index.row()), column));
    case Qt::TextAlignmentRole:
        return QVariant::fromValue(alignment(column));
    case Qt::FontRole:
        return column == ui::TripColumn::total ? QVariant::fromValue(bold_) : QVariant{};
    default:
        return {};
    }
}

QVariant RoundTripTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < columnCount()) {
        return QString::fromStdString(grid_.header(column_at(section)));
    }
    return QAbstractTableModel::headerData(section, orientation, role);
}

Qt::ItemFlags RoundTripTableModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren;  // read-only
}

void RoundTripTableModel::set_trips(std::vector<RoundTrip> trips, std::string_view currency,
                                    std::string_view summary)
{
    const std::string currency_before = grid_.currency();
    reset_with([&] { grid_.set_trips(std::move(trips), currency, summary); });
    if (grid_.currency() != currency_before) {
        const int total = column_index(ui::TripColumn::total);
        Q_EMIT headerDataChanged(Qt::Horizontal, total, total);
    }
}

void RoundTripTableModel::sort_by_column(ui::TripColumn column)
{
    if (column == ui::TripColumn::idx) {
        return;  // hidden: no header to click
    }
    relayout_by_key([this](int row) { return grid_.row(to_size(row)).idx; },
                    [&] { grid_.sort_by_column(column); });
}

void RoundTripTableModel::mouse_down(int row, ui::MouseButton button)
{
    if (row >= 0) {
        grid_.mouse_down(to_size(row), button);
    }
}

void RoundTripTableModel::set_current_row(int row)
{
    if (row >= 0) {
        grid_.set_current_row(to_size(row));
    }
}

} // namespace wizz::app
