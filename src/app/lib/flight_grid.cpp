#include "flight_grid.hpp"

#include "grid_table_view.hpp"
#include "grid_top_bar.hpp"
#include "modern_style.hpp"

#include "wizz/ui/strings.hpp"

#include <QAction>
#include <QComboBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QScopedValueRollback>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace wizz::app {

namespace {

using ui::FlightColumn;
using ui::ScrollRequest;

/// Widths of the visible columns (01 §8.2, logical pixels).
struct ColumnWidth {
    FlightColumn column;
    int width;
};
constexpr std::array kColumnWidths{
    ColumnWidth{FlightColumn::date, 124},          ColumnWidth{FlightColumn::departs, 64},
    ColumnWidth{FlightColumn::from, 160},          ColumnWidth{FlightColumn::km_from_start, 70},
    ColumnWidth{FlightColumn::to, 180},            ColumnWidth{FlightColumn::km_from_dest, 70},
    ColumnWidth{FlightColumn::total, 92},          ColumnWidth{FlightColumn::per_person, 84},
    ColumnWidth{FlightColumn::fare_local, 110},    ColumnWidth{FlightColumn::all_departures, 170},
    ColumnWidth{FlightColumn::notes, 260},
};
/// Notes fills the remaining width, min L(120).
constexpr int kNotesMinimumWidth = 120;
/// `_view` width L(190).
constexpr int kViewComboWidth = 190;

[[nodiscard]] ui::MouseButton to_ui(Qt::MouseButton button) noexcept
{
    switch (button) {
    case Qt::LeftButton:
        return ui::MouseButton::left;
    case Qt::RightButton:
        return ui::MouseButton::right;
    default:
        return ui::MouseButton::middle;  // any other button picks without moving the current row
    }
}

[[nodiscard]] int section(FlightColumn column) noexcept
{
    return FlightTableModel::column_index(column);
}

} // namespace

FlightGrid::FlightGrid(std::shared_ptr<const Locale> locale, bool offer_plan_return, QWidget* parent)
    : QWidget(parent), actions_(GridActions::system())
{
    model_ = new FlightTableModel(std::move(locale), offer_plan_return, this);

    // Top bar (01 §8.1).
    auto* show = new QLabel(grid_bar::text(ui::strings::kShowLabel), this);
    show->setTextFormat(Qt::PlainText);
    view_ = new QComboBox(this);
    for (const std::string_view item : ui::strings::kViewItems) {
        view_->addItem(grid_bar::text(item));
    }
    view_->setCurrentIndex(0);
    view_->setFixedWidth(kViewComboWidth);
    ModernStyle::use_styled_popup(*view_);
    summary_ = grid_bar::make_summary_label(this);
    QHBoxLayout* bar = grid_bar::make_row();
    grid_bar::add(bar, show, grid_bar::kLabelMargin);
    grid_bar::add(bar, view_, grid_bar::kDefaultMargin);
    grid_bar::add(bar, summary_, grid_bar::kSummaryMargin, 1);

    // Grid.
    table_ = new GridTableView(this);
    table_->setModel(model_);
    build_columns();
    setTabOrder(table_, view_);  // the grid before its view combo (01 §6.3)

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(table_, 1);

    // Context menu (01 §8.9).
    menu_ = new QMenu(this);
    open_ = menu_->addAction(grid_bar::text(ui::strings::kOpenOnWizzair));
    copy_ = menu_->addAction(grid_bar::text(ui::strings::kCopyDetails));
    if (offer_plan_return) {
        menu_->addSeparator();
        plan_return_ = menu_->addAction(grid_bar::text(ui::strings::kPlanReturnMenu));
        connect(plan_return_, &QAction::triggered, this, &FlightGrid::request_plan_return);
    }
    connect(open_, &QAction::triggered, this, &FlightGrid::open_selected);
    connect(copy_, &QAction::triggered, this, &FlightGrid::copy_selected);

    clipboard_retry_.setSingleShot(true);
    clipboard_retry_.setInterval(kClipboardRetryDelay);
    connect(&clipboard_retry_, &QTimer::timeout, this, &FlightGrid::try_copy);

    connect(view_, &QComboBox::activated, this, &FlightGrid::on_view_activated);  // user changes only
    connect(table_->horizontalHeader(), &QHeaderView::sectionClicked, this, &FlightGrid::on_header_clicked);
    connect(table_, &GridTableView::data_cell_pressed, this, &FlightGrid::on_cell_pressed);
    connect(table_, &GridTableView::data_cell_double_clicked, this, [this](int) { open_selected(); });
    connect(table_, &GridTableView::enter_pressed, this, &FlightGrid::open_selected);
    connect(table_, &GridTableView::navigation_key_released, this, [this] { model_->navigation_key_up(); });
    connect(table_, &GridTableView::context_menu_requested, this, &FlightGrid::show_context_menu);
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& current, const QModelIndex&) { on_current_row_changed(current); });

    after_change();
}

FlightGrid::~FlightGrid() = default;

void FlightGrid::clear(std::string_view currency, bool anywhere, std::string_view summary)
{
    {
        const QScopedValueRollback guard{syncing_, true};
        model_->clear(currency, anywhere, summary);
    }
    after_change();
    Q_EMIT contents_changed();
}

void FlightGrid::append(std::span<const FlightResultPtr> batch)
{
    if (std::ranges::none_of(batch, [](const FlightResultPtr& fare) { return fare != nullptr; })) {
        return;  // an empty flush changes nothing
    }
    {
        const QScopedValueRollback guard{syncing_, true};
        model_->append(batch);
    }
    after_change();
    Q_EMIT contents_changed();
}

void FlightGrid::set_results(std::span<const FlightResultPtr> results, std::string_view currency,
                             std::string_view summary)
{
    {
        const QScopedValueRollback guard{syncing_, true};
        model_->set_results(results, currency, summary);
    }
    after_change();
    Q_EMIT contents_changed();
}

void FlightGrid::set_summary(std::string_view summary)
{
    model_->set_summary(summary);
    const QString text = grid_bar::text(summary);
    if (summary_->text() != text) {
        summary_->setText(text);
    }
}

FlightResultPtr FlightGrid::selected() const
{
    return model_->state().selected();
}

FlightResultPtr FlightGrid::explicit_selection() const
{
    return model_->state().explicit_selection();
}

std::span<const FlightResultPtr> FlightGrid::all_results() const
{
    return model_->state().all_results();
}

std::span<const FlightResultPtr> FlightGrid::shown_results() const
{
    return model_->state().shown_results();
}

ui::ViewMode FlightGrid::mode() const
{
    return model_->state().mode();
}

std::size_t FlightGrid::row_count() const
{
    return model_->state().row_count();
}

const std::string& FlightGrid::summary_text() const
{
    return model_->state().summary();
}

std::vector<std::string> FlightGrid::visible_rows_text(std::size_t max) const
{
    return model_->state().visible_rows_text(max);
}

std::string FlightGrid::current_row_text() const
{
    return model_->state().current_row_text();
}

void FlightGrid::set_busy_query(std::function<bool()> is_busy)
{
    is_busy_ = std::move(is_busy);
}

void FlightGrid::set_actions(GridActions actions)
{
    actions_ = std::move(actions);
}

void FlightGrid::open_selected()
{
    const FlightResultPtr fare = selected();
    if (!fare) {
        return;
    }
    if (const auto opened = open_url_or_warn(actions_, fare->booking_url); !opened) {
        Q_EMIT open_url_failed(QString::fromStdString(fare->booking_url), opened.error());
    }
}

void FlightGrid::copy_selected()
{
    const std::optional<std::string> text = model_->state().copy_selected_text();
    if (!text) {
        return;
    }
    clipboard_retry_.stop();
    pending_copy_ = QString::fromStdString(*text);
    copy_attempts_left_ = kClipboardAttempts;
    try_copy();
}

void FlightGrid::request_plan_return()
{
    if (FlightResultPtr fare = selected()) {
        Q_EMIT plan_return_requested(std::move(fare));
    }
}

QMenu* FlightGrid::prepare_context_menu()
{
    const ui::FlightMenuState state = model_->state().context_menu(is_busy_ && is_busy_());
    open_->setEnabled(state.open_enabled);
    copy_->setEnabled(state.copy_enabled);
    if (plan_return_ != nullptr) {
        const QString text = grid_bar::text(state.plan_return_text);
        if (plan_return_->text() != text) {
            plan_return_->setText(text);
        }
        plan_return_->setEnabled(state.plan_return_enabled);
    }
    return menu_;
}

void FlightGrid::build_columns()
{
    for (const ColumnWidth& column : kColumnWidths) {
        table_->setColumnWidth(section(column.column), column.width);
    }
    table_->setColumnHidden(section(FlightColumn::idx), true);
    table_->setColumnHidden(section(FlightColumn::price_sort), true);
    table_->set_fill_column(section(FlightColumn::notes), kNotesMinimumWidth);
}

void FlightGrid::on_view_activated(int index)
{
    if (index < 0 || index >= static_cast<int>(ui::strings::kViewItems.size())) {
        return;
    }
    {
        const QScopedValueRollback guard{syncing_, true};
        model_->choose_view(static_cast<ui::ViewMode>(index));  // Rebuild(): restores the pick or the top
    }
    after_change();
}

void FlightGrid::on_header_clicked(int clicked)
{
    if (clicked >= 0 && clicked < model_->columnCount()) {
        const QScopedValueRollback guard{syncing_, true};
        model_->sort_by_column(FlightTableModel::column_at(clicked));
    }
    update_sort_indicator();
    sync_current_row();
}

void FlightGrid::on_cell_pressed(int row, Qt::MouseButton button)
{
    model_->mouse_down(row, to_ui(button));  // _pickedFare = RowFare(row) ?? _pickedFare
}

void FlightGrid::on_current_row_changed(const QModelIndex& current)
{
    if (!syncing_ && current.isValid()) {
        model_->set_current_row(current.row());
    }
}

void FlightGrid::show_context_menu(QPoint global_pos)
{
    prepare_context_menu()->popup(global_pos);
}

void FlightGrid::try_copy()
{
    if (copy_attempts_left_ <= 0) {
        return;
    }
    --copy_attempts_left_;
    if (!actions_.try_set_clipboard || actions_.try_set_clipboard(pending_copy_)) {
        copy_attempts_left_ = 0;
        return;
    }
    if (copy_attempts_left_ > 0) {
        clipboard_retry_.start();  // another program holds the clipboard: try again shortly
        return;
    }
    if (actions_.show_message) {
        actions_.show_message(QMessageBox::Information, grid_bar::text(ui::strings::kClipboardBusy));
    }
}

void FlightGrid::after_change()
{
    update_headers();
    const int mode_index = static_cast<int>(model_->state().mode());
    if (view_->currentIndex() != mode_index) {
        view_->setCurrentIndex(mode_index);  // a code change: no `activated`, not a user choice
    }
    const QString summary = grid_bar::text(model_->state().summary());
    if (summary_->text() != summary) {
        summary_->setText(summary);
    }
    update_sort_indicator();
    sync_current_row();
}

void FlightGrid::update_headers()
{
    // The header texts come from the model; "km from dest." shows only when not anywhere.
    // Properties change only when the value differs (each change can relayout, WF-2).
    const int km = section(FlightColumn::km_from_dest);
    const bool hidden = !model_->state().column_visible(FlightColumn::km_from_dest);
    if (table_->isColumnHidden(km) != hidden) {
        table_->setColumnHidden(km, hidden);
        table_->fit_fill_column();
    }
}

void FlightGrid::update_sort_indicator()
{
    const auto& sort = model_->state().user_sort();
    table_->show_sort_indicator(sort ? section(sort->column) : -1,
                                sort && sort->direction == ui::SortDirection::descending ? Qt::DescendingOrder
                                                                                         : Qt::AscendingOrder);
}

void FlightGrid::sync_current_row()
{
    const ScrollRequest request = model_->take_scroll_request();
    const bool keep_horizontal = model_->take_keep_horizontal_scroll();
    const std::optional<std::size_t> row = model_->state().current_row();
    const QScopedValueRollback guard{syncing_, true};
    table_->show_current_row(row ? std::optional<int>{static_cast<int>(*row)} : std::nullopt, request,
                             section(FlightColumn::date), keep_horizontal);
}

} // namespace wizz::app
