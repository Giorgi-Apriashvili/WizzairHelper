#include "round_trip_grid.hpp"

#include "grid_table_view.hpp"
#include "grid_top_bar.hpp"

#include "wizz/ui/strings.hpp"

#include <QAction>
#include <QCheckBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QScopedValueRollback>
#include <QVBoxLayout>

#include <array>
#include <optional>
#include <utility>

namespace wizz::app {

namespace {

using ui::ScrollRequest;
using ui::TripColumn;

/// Widths of the visible columns (01 §9.2, logical pixels).
struct ColumnWidth {
    TripColumn column;
    int width;
};
constexpr std::array kColumnWidths{
    ColumnWidth{TripColumn::total, 95},        ColumnWidth{TripColumn::nights, 55},
    ColumnWidth{TripColumn::out_date, 95},     ColumnWidth{TripColumn::outbound, 300},
    ColumnWidth{TripColumn::out_price, 80},    ColumnWidth{TripColumn::return_date, 95},
    ColumnWidth{TripColumn::return_leg, 300},  ColumnWidth{TripColumn::return_price, 100},
    ColumnWidth{TripColumn::booking_notes, 260},
};
/// "Booking / notes" fills the remaining width, at least L(120): at 5 px its header wrapped
/// word by word into a three-line header row (D-53). "Return price" is 100 wide so the
/// semibold header stays on one line, which keeps one more row in view.
constexpr int kNotesMinimumWidth = 120;

[[nodiscard]] int section(TripColumn column) noexcept
{
    return RoundTripTableModel::column_index(column);
}

[[nodiscard]] QCheckBox* make_filter_box(std::string_view text, QWidget* parent)
{
    auto* box = new QCheckBox(grid_bar::text(text), parent);
    box->setChecked(false);
    return box;
}

} // namespace

RoundTripGrid::RoundTripGrid(std::shared_ptr<const Locale> locale, QWidget* parent)
    : QWidget(parent), actions_(GridActions::system())
{
    model_ = new RoundTripTableModel(std::move(locale), this);

    // Top bar (01 §9.1).
    same_home_ = make_filter_box(ui::strings::kSameHomeCheckbox, this);
    same_away_ = make_filter_box(ui::strings::kSameAwayCheckbox, this);
    summary_ = grid_bar::make_summary_label(this);
    QHBoxLayout* bar = grid_bar::make_row();
    grid_bar::add(bar, same_home_, grid_bar::kCheckBoxMargin);
    grid_bar::add(bar, same_away_, grid_bar::kCheckBoxMargin);
    grid_bar::add(bar, summary_, grid_bar::kSummaryMargin, 1);

    // Grid.
    table_ = new GridTableView(this);
    table_->setModel(model_);
    for (const ColumnWidth& column : kColumnWidths) {
        table_->setColumnWidth(section(column.column), column.width);
    }
    table_->setColumnHidden(section(TripColumn::idx), true);
    table_->set_fill_column(section(TripColumn::booking_notes), kNotesMinimumWidth);
    setTabOrder({table_, same_home_, same_away_});  // the grid before its filters (01 §6.3)

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(table_, 1);

    // Context menu (01 §9.4): always enabled.
    menu_ = new QMenu(this);
    connect(menu_->addAction(grid_bar::text(ui::strings::kOpenOnWizzair)), &QAction::triggered, this,
            &RoundTripGrid::open_selected);
    connect(menu_->addAction(grid_bar::text(ui::strings::kOpenOutboundOnly)), &QAction::triggered, this,
            &RoundTripGrid::open_outbound);
    connect(menu_->addAction(grid_bar::text(ui::strings::kOpenReturnOnly)), &QAction::triggered, this,
            &RoundTripGrid::open_return);

    connect(same_home_, &QCheckBox::toggled, this, [this](bool checked) {
        if (model_->set_require_same_home(checked)) {
            Q_EMIT filters_changed();
        }
    });
    connect(same_away_, &QCheckBox::toggled, this, [this](bool checked) {
        if (model_->set_require_same_away(checked)) {
            Q_EMIT filters_changed();
        }
    });
    connect(table_->horizontalHeader(), &QHeaderView::sectionClicked, this, &RoundTripGrid::on_header_clicked);
    connect(table_, &GridTableView::data_cell_pressed, this, &RoundTripGrid::on_cell_pressed);
    connect(table_, &GridTableView::data_cell_double_clicked, this, [this](int) { open_selected(); });
    connect(table_, &GridTableView::enter_pressed, this, &RoundTripGrid::open_selected);
    connect(table_, &GridTableView::context_menu_requested, this, [this](QPoint pos) { menu_->popup(pos); });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& current, const QModelIndex&) {
                if (!syncing_ && current.isValid()) {
                    model_->set_current_row(current.row());
                }
            });
}

RoundTripGrid::~RoundTripGrid() = default;

void RoundTripGrid::set_trips(std::vector<RoundTrip> trips, std::string_view currency, std::string_view summary)
{
    {
        const QScopedValueRollback guard{syncing_, true};
        model_->set_trips(std::move(trips), currency, summary);
    }
    const QString text = grid_bar::text(model_->state().summary_text());
    if (summary_->text() != text) {
        summary_->setText(text);
    }
    update_sort_indicator();
    sync_current_row();
}

std::size_t RoundTripGrid::count() const
{
    return model_->state().count();
}

std::span<const RoundTrip> RoundTripGrid::trips() const
{
    return model_->state().trips();
}

const std::string& RoundTripGrid::summary_text() const
{
    return model_->state().summary_text();
}

bool RoundTripGrid::require_same_home() const
{
    return same_home_->isChecked();
}

bool RoundTripGrid::require_same_away() const
{
    return same_away_->isChecked();
}

const RoundTrip* RoundTripGrid::selected() const
{
    return model_->state().selected();
}

void RoundTripGrid::set_actions(GridActions actions)
{
    actions_ = std::move(actions);
}

void RoundTripGrid::open_selected()
{
    open_urls(model_->state().open_selected_urls());
}

void RoundTripGrid::open_outbound()
{
    if (const std::optional<std::string> url = model_->state().outbound_url()) {
        open_urls({*url});
    }
}

void RoundTripGrid::open_return()
{
    if (const std::optional<std::string> url = model_->state().return_url()) {
        open_urls({*url});
    }
}

void RoundTripGrid::open_urls(const std::vector<std::string>& urls)
{
    for (const std::string& url : urls) {  // two one-way bookings open as two browser tabs
        if (const auto opened = open_url_or_warn(actions_, url); !opened) {
            Q_EMIT open_url_failed(QString::fromStdString(url), opened.error());
        }
    }
}

void RoundTripGrid::on_header_clicked(int clicked)
{
    if (clicked >= 0 && clicked < model_->columnCount()) {
        const QScopedValueRollback guard{syncing_, true};
        model_->sort_by_column(RoundTripTableModel::column_at(clicked));
    }
    update_sort_indicator();
    sync_current_row();
}

void RoundTripGrid::on_cell_pressed(int row, Qt::MouseButton button)
{
    const ui::MouseButton ui_button = button == Qt::LeftButton    ? ui::MouseButton::left
                                      : button == Qt::RightButton ? ui::MouseButton::right
                                                                  : ui::MouseButton::middle;
    model_->mouse_down(row, ui_button);
}

void RoundTripGrid::update_sort_indicator()
{
    const auto& sort = model_->state().user_sort();
    table_->show_sort_indicator(sort ? section(sort->column) : -1,
                                sort && sort->direction == ui::SortDirection::descending ? Qt::DescendingOrder
                                                                                         : Qt::AscendingOrder);
}

void RoundTripGrid::sync_current_row()
{
    const ScrollRequest request = model_->take_scroll_request();
    const std::optional<std::size_t> row = model_->state().current_row();
    const QScopedValueRollback guard{syncing_, true};
    table_->show_current_row(row ? std::optional<int>{static_cast<int>(*row)} : std::nullopt, request,
                             section(TripColumn::total));
}

} // namespace wizz::app
