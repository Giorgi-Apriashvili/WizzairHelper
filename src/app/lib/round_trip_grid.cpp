#include "round_trip_grid.hpp"

#include "elided_label.hpp"
#include "grid_bar_button.hpp"
#include "grid_table_view.hpp"
#include "grid_top_bar.hpp"
#include "ui_style.hpp"

#include "wizz/ui/strings.hpp"

#include <QAction>
#include <QCheckBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
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
    homes_ = new GridBarButton(style::glyph::kHome, grid_bar::text(ui::strings::kHomesOnAirbnb),
                               grid_bar::text(ui::strings::kHomesToolTip), this);
    homes_->setObjectName(QStringLiteral("homesButton"));
    QHBoxLayout* bar = grid_bar::make_row();
    grid_bar::add(bar, same_home_, grid_bar::kCheckBoxMargin);
    grid_bar::add(bar, same_away_, grid_bar::kCheckBoxMargin);
    grid_bar::add(bar, summary_, grid_bar::kSummaryMargin, 1);
    grid_bar::add(bar, homes_, grid_bar::kActionMargin);

    // Grid.
    table_ = new GridTableView(this);
    table_->setModel(model_);
    for (const ColumnWidth& column : kColumnWidths) {
        table_->setColumnWidth(section(column.column), column.width);
    }
    table_->setColumnHidden(section(TripColumn::idx), true);
    table_->set_fill_column(section(TripColumn::booking_notes), kNotesMinimumWidth);
    setTabOrder({table_, same_home_, same_away_, homes_});  // the grid before its bar (01 §6.3)

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(table_, 1);

    // Context menu (01 §9.4): the three booking items are always enabled; the two copy items
    // (D-76) copy the selected trips; "Homes on Airbnb" (D-72) follows the current trip.
    menu_ = new QMenu(this);
    connect(menu_->addAction(grid_bar::text(ui::strings::kOpenOnWizzair)), &QAction::triggered, this,
            &RoundTripGrid::open_selected);
    connect(menu_->addAction(grid_bar::text(ui::strings::kOpenOutboundOnly)), &QAction::triggered, this,
            &RoundTripGrid::open_outbound);
    connect(menu_->addAction(grid_bar::text(ui::strings::kOpenReturnOnly)), &QAction::triggered, this,
            &RoundTripGrid::open_return);
    menu_->addSeparator();
    copy_action_ = menu_->addAction(grid_bar::text(ui::strings::kCopyDetails));
    copy_action_->setShortcut(QKeySequence::Copy);  // shown in the menu; the table handles Ctrl+C itself
    copy_action_->setShortcutContext(Qt::WidgetShortcut);
    connect(copy_action_, &QAction::triggered, this, &RoundTripGrid::copy_selected);
    copy_table_action_ = menu_->addAction(grid_bar::text(ui::strings::kCopyAsTable));
    connect(copy_table_action_, &QAction::triggered, this, &RoundTripGrid::copy_table);
    clipboard_ = new ClipboardCopy(actions_, this);
    menu_->addSeparator();
    homes_action_ = menu_->addAction(grid_bar::text(ui::strings::kHomesOnAirbnb));
    connect(homes_action_, &QAction::triggered, this, &RoundTripGrid::request_homes);
    connect(homes_, &QPushButton::clicked, this, &RoundTripGrid::request_homes);

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
    connect(table_, &GridTableView::copy_requested, this, &RoundTripGrid::copy_selected);
    connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this] { on_selection_changed(); });
    connect(table_, &GridTableView::context_menu_requested, this,
            [this](QPoint pos) { prepare_context_menu()->popup(pos); });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& current, const QModelIndex&) {
                if (!syncing_ && current.isValid()) {
                    model_->set_current_row(current.row());
                    refresh_homes();
                }
            });
    refresh_homes();
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

void RoundTripGrid::set_today_query(std::function<Date()> today)
{
    today_ = std::move(today);
    refresh_homes();
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

std::optional<ui::HomesStay> RoundTripGrid::selected_stay() const
{
    const RoundTrip* trip = selected();
    return trip != nullptr ? ui::stay_for_trip(*trip, today_ ? today_() : Date::min_value()) : std::nullopt;
}

void RoundTripGrid::request_homes()
{
    if (const std::optional<ui::HomesStay> stay = selected_stay()) {
        Q_EMIT homes_requested(*stay);
    }
}

QMenu* RoundTripGrid::prepare_context_menu()
{
    const bool rows = model_->state().row_count() > 0;  // the copy items need a trip (D-76)
    copy_action_->setEnabled(rows);
    copy_table_action_->setEnabled(rows);
    homes_action_->setEnabled(selected_stay().has_value());
    refresh_homes();
    return menu_;
}

void RoundTripGrid::refresh_homes()
{
    const bool enabled = selected_stay().has_value();
    if (homes_->isEnabled() != enabled) {
        homes_->setEnabled(enabled);
    }
}

void RoundTripGrid::copy_selected()
{
    if (const std::optional<std::string> text = model_->state().copy_selected_text()) {
        clipboard_->copy(QString::fromStdString(*text));
    }
}

void RoundTripGrid::copy_table()
{
    if (const std::optional<std::string> text = model_->state().copy_table_text()) {
        clipboard_->copy(QString::fromStdString(*text));
    }
}

void RoundTripGrid::on_selection_changed()
{
    if (!syncing_) {
        const std::vector<std::size_t> rows = table_->selected_rows();
        const QModelIndex current = table_->selectionModel()->currentIndex();  // already the new one
        model_->select_rows(rows, current.isValid() ? std::optional<std::size_t>{static_cast<std::size_t>(current.row())}
                                                    : std::nullopt);  // the user's selection, kept across sorting (D-76)
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
    {
        const QScopedValueRollback guard{syncing_, true};
        const std::vector<std::size_t> selected = model_->state().selected_rows();
        table_->show_current_row(row ? std::optional<int>{static_cast<int>(*row)} : std::nullopt, selected, request,
                                 section(TripColumn::total));
    }
    refresh_homes();
}

} // namespace wizz::app
