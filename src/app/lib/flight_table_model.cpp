#include "flight_table_model.hpp"

#include "grid_colors.hpp"
#include "grid_row_delegate.hpp"
#include "ui_style.hpp"

#include <QScopeGuard>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace wizz::app {

namespace {

// Model columns are the data columns of 01 §8.2 in order, so the enum value is the section.
static_assert([] {
    for (std::size_t i = 0; i < ui::kFlightColumnCount; ++i) {
        if (ui::kFlightColumns[i] != static_cast<ui::FlightColumn>(i)) {
            return false;
        }
    }
    return true;
}());

[[nodiscard]] constexpr std::size_t to_size(int value) noexcept
{
    return static_cast<std::size_t>(value);
}

[[nodiscard]] constexpr bool is_set(const FlightResultPtr& flight) noexcept
{
    return flight != nullptr;
}

} // namespace

FlightTableModel::FlightTableModel(std::shared_ptr<const Locale> locale, bool offer_plan_return, QObject* parent)
    : KeyedTableModel(parent), grid_(std::move(locale), offer_plan_return)
{
    bold_.setWeight(QFont::DemiBold);  // only the weight is set: it resolves against the view's font
    apply_theme();
    style::on_theme_change(this, [this] { apply_theme(); });  // the views repaint and ask again
}

void FlightTableModel::apply_theme()
{
    gray_text_ = QBrush{grid_colors::gray_text()};
    cheapest_ = QBrush{grid_colors::cheapest_back()};
    cheapest_selection_ = QBrush{grid_colors::cheapest_selection_back()};
}

std::size_t FlightTableModel::idx_at(int row) const
{
    return grid_.row(to_size(row)).idx;
}

int FlightTableModel::column_index(ui::FlightColumn column) noexcept
{
    return static_cast<int>(column);
}

ui::FlightColumn FlightTableModel::column_at(int section) noexcept
{
    return ui::kFlightColumns[to_size(section)];
}

Qt::Alignment FlightTableModel::alignment(ui::FlightColumn column) noexcept
{
    switch (column) {
    case ui::FlightColumn::departs:
        return Qt::AlignHCenter | Qt::AlignVCenter;
    case ui::FlightColumn::km_from_start:
    case ui::FlightColumn::km_from_dest:
    case ui::FlightColumn::total:
    case ui::FlightColumn::per_person:
    case ui::FlightColumn::fare_local:
        return Qt::AlignRight | Qt::AlignVCenter;
    default:
        return Qt::AlignLeft | Qt::AlignVCenter;
    }
}

int FlightTableModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(grid_.row_count());
}

int FlightTableModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(ui::kFlightColumnCount);
}

QVariant FlightTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount() || index.column() >= columnCount()) {
        return {};
    }
    const std::size_t row = to_size(index.row());
    const ui::FlightColumn column = column_at(index.column());
    switch (role) {
    case Qt::DisplayRole:
        return QString::fromStdString(grid_.cell_text(row, column));
    case Qt::TextAlignmentRole:
        return QVariant::fromValue(alignment(column));
    case Qt::FontRole:
        return column == ui::FlightColumn::total ? QVariant::fromValue(bold_) : QVariant{};
    case Qt::ForegroundRole:
        return grid_.tone(row) == ui::RowTone::unavailable ? QVariant::fromValue(gray_text_) : QVariant{};
    case Qt::BackgroundRole:
        return grid_.tone(row) == ui::RowTone::cheapest ? QVariant::fromValue(cheapest_) : QVariant{};
    case GridRowDelegate::SelectionBackgroundRole:
        return grid_.tone(row) == ui::RowTone::cheapest ? QVariant::fromValue(cheapest_selection_) : QVariant{};
    case RowToneRole:
        return static_cast<int>(grid_.tone(row));
    default:
        return {};
    }
}

QVariant FlightTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole && section >= 0 && section < columnCount()) {
        return QString::fromStdString(grid_.header(column_at(section)));
    }
    if (orientation == Qt::Horizontal && role == Qt::TextAlignmentRole && section >= 0 && section < columnCount()) {
        // Headers line up with their values (numbers right, times centred), wrapped.
        const Qt::Alignment horizontal = alignment(column_at(section)) & Qt::AlignHorizontal_Mask;
        return static_cast<int>(horizontal.toInt() | Qt::AlignVCenter | Qt::TextWordWrap);
    }
    return QAbstractTableModel::headerData(section, orientation, role);
}

Qt::ItemFlags FlightTableModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren;  // read-only
}

void FlightTableModel::clear(std::string_view currency, bool anywhere, std::string_view summary)
{
    const std::string currency_before = grid_.currency();
    reset_with([&] { grid_.clear(currency, anywhere, summary); });
    notify_currency(currency_before);
}

void FlightTableModel::append(std::span<const FlightResultPtr> batch)
{
    const auto added = std::ranges::count_if(batch, is_set);
    if (added == 0) {
        return;
    }
    if (grid_.mode() != ui::ViewMode::all_fares) {
        keep_horizontal_scroll_ = grid_.explicit_selection() == nullptr;  // Rebuild(false): no RestoreSelection
        reset_with([&] { grid_.append(batch); });  // grouped views rebuild on every flush
        return;
    }
    if (grid_.row_count() == 0) {
        beginInsertRows({}, 0, static_cast<int>(added) - 1);
        const auto end = qScopeGuard([this] { endInsertRows(); });
        grid_.append(batch);
        return;
    }
    const Decimal cheapest_before = grid_.cheapest();
    relayout_by_key([this](int row) { return idx_at(row); }, [&] { grid_.append(batch); });
    if (grid_.cheapest() != cheapest_before) {
        // The highlight moves to the new cheapest rows: repaint every row (01 §8.3).
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
                           {Qt::BackgroundRole, RowToneRole});
    }
}

void FlightTableModel::set_results(std::span<const FlightResultPtr> results, std::string_view currency,
                                   std::string_view summary)
{
    const std::string currency_before = grid_.currency();
    reset_with([&] { grid_.set_results(results, currency, summary); });
    notify_currency(currency_before);
}

void FlightTableModel::set_summary(std::string_view summary)
{
    grid_.set_summary(summary);
}

void FlightTableModel::choose_view(ui::ViewMode mode)
{
    if (mode == grid_.mode()) {
        return;  // choosing the shown view changes nothing (no SelectedIndexChanged)
    }
    reset_with([&] { grid_.choose_view(mode); });
}

void FlightTableModel::sort_by_column(ui::FlightColumn column)
{
    if (!grid_.column_visible(column)) {
        return;  // hidden columns have no header to click
    }
    relayout_by_key([this](int row) { return idx_at(row); }, [&] { grid_.sort_by_column(column); });
}

void FlightTableModel::mouse_down(int row, ui::MouseButton button)
{
    if (row >= 0) {
        grid_.mouse_down(to_size(row), button);
    }
}

void FlightTableModel::set_current_row(int row)
{
    if (row >= 0) {
        grid_.set_current_row(to_size(row));
    }
}

void FlightTableModel::navigation_key_up()
{
    grid_.navigation_key_up();
}

FlightResultPtr FlightTableModel::fare_at(int row) const
{
    return row >= 0 && row < rowCount() ? grid_.row(to_size(row)).fare : FlightResultPtr{};
}

void FlightTableModel::notify_currency(const std::string& before)
{
    if (grid_.currency() != before) {
        Q_EMIT headerDataChanged(Qt::Horizontal, column_index(ui::FlightColumn::total),
                                 column_index(ui::FlightColumn::per_person));
    }
}

} // namespace wizz::app
