#include "input_panel.hpp"

#include "date_picker.hpp"
#include "flow_layout.hpp"
#include "fractional_spin_box.hpp"
#include "place_combo.hpp"
#include "qt_strings.hpp"
#include "search_button.hpp"
#include "ui_style.hpp"

#include "wizz/core/text.hpp"
#include "wizz/ui/strings.hpp"

#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QScopedValueRollback>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace wizz::app {

namespace {

namespace strings = ui::strings;

// 01 §4.3 / §4.4 geometry (logical pixels).
constexpr QMargins kPanelPadding{10, 8, 10, 4};
constexpr QMargins kRowMargins{0, 1, 0, 1};
constexpr QMargins kLabelMargins{3, 7, 3, 0};
constexpr QMargins kHintMargins{10, 7, 3, 0};
constexpr QMargins kResolvedMargins{3, 4, 3, 2};
constexpr int kPlaceWidth = 210;
constexpr int kRadiusWidth = 64;
constexpr int kDateWidth = 130;
constexpr int kAdultsWidth = 48;
constexpr int kFareWidth = 175;
constexpr int kCurrencyWidth = 70;
constexpr int kNightsWidth = 52;
constexpr int kWideGap = 24;
constexpr int kSmallGap = 8;
constexpr int kButtonGap = 16;
constexpr double kRadiusStep = 25;
constexpr int kMaxAdults = 9;

[[nodiscard]] QLabel* make_label(std::string_view text, QWidget* parent)
{
    auto* label = new QLabel(to_qstring(text), parent);
    label->setTextFormat(Qt::PlainText);
    return label;
}

void make_gray(QWidget& widget)
{
    QPalette palette = widget.palette();
    palette.setColor(QPalette::WindowText, style::gray_text());
    widget.setPalette(palette);
}

/// `Lbl(text)`.
void add_label(FlowLayout& row, std::string_view text)
{
    row.add_widget(make_label(text, row.parentWidget()), kLabelMargins);
}

/// `Hint(text)`.
void add_hint(FlowLayout& row, std::string_view text)
{
    QLabel* hint = make_label(text, row.parentWidget());
    make_gray(*hint);
    row.add_widget(hint, kHintMargins);
}

[[nodiscard]] FractionalSpinBox* make_numeric(QWidget* parent, const NumberSymbols& symbols, double minimum,
                                              double maximum, double step, int width)
{
    auto* box = new FractionalSpinBox(parent);
    box->set_number_symbols(symbols);
    box->set_range(minimum, maximum);
    box->set_single_step(step);
    box->setFixedWidth(width);
    return box;
}

[[nodiscard]] int currency_index(std::string_view code)
{
    const auto found = std::ranges::find(strings::kCurrencies, code);
    return found == strings::kCurrencies.end() ? 0 : static_cast<int>(found - strings::kCurrencies.begin());
}

} // namespace

InputPanel::InputPanel(std::shared_ptr<const Locale> locale, Date today, const ui::AppSettings& settings,
                       QWidget* parent)
    : QWidget(parent), locale_(std::move(locale)), rules_(ui::DateRules::for_today(today))
{
    if (!locale_) {
        throw std::invalid_argument("InputPanel needs a locale");
    }
    build();
    load_from(settings);  // BuildInputs: defaults first …
    wire();               // … then WireDateRules (01 §4.4 notes)
    apply_tab_order();
}

InputPanel::~InputPanel() = default;

void InputPanel::build()
{
    const NumberSymbols& symbols = locale_->number_symbols();
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(kPanelPadding);
    outer->setSpacing(0);
    for (int i = 0; i < 3; ++i) {
        auto* row = new QWidget(this);
        row->setObjectName(QStringLiteral("row%1").arg(i + 1));
        auto* layout = new FlowLayout(row);
        layout->setContentsMargins(kRowMargins);
        rows_[i] = row;
        row_layouts_[i] = layout;
        outer->addWidget(row);
    }

    // Row 1, where.
    FlowLayout& row1 = *row_layouts_[0];
    QWidget* const parent1 = rows_[0];
    from_ = new PlaceCombo(parent1);
    from_->setFixedWidth(kPlaceWidth);
    from_->setToolTip(to_qstring(strings::kTooltipFrom));
    from_radius_ = make_numeric(parent1, symbols, 0, ui::kMaxRadiusKm, kRadiusStep, kRadiusWidth);
    to_ = new PlaceCombo(parent1);
    to_->setFixedWidth(kPlaceWidth);
    to_->setToolTip(to_qstring(strings::kTooltipTo));
    to_radius_ = make_numeric(parent1, symbols, 0, ui::kMaxRadiusKm, kRadiusStep, kRadiusWidth);
    add_label(row1, strings::kLabelFrom);
    row1.add_widget(from_);
    add_label(row1, strings::kLabelWithin);
    row1.add_widget(from_radius_);
    add_label(row1, strings::kLabelKm);
    row1.add_gap(kWideGap);
    add_label(row1, strings::kLabelTo);
    row1.add_widget(to_);
    add_label(row1, strings::kLabelWithin);
    row1.add_widget(to_radius_);
    add_label(row1, strings::kLabelKm);
    add_hint(row1, strings::kHintAnywhere);

    // Row 2, outbound.
    FlowLayout& row2 = *row_layouts_[1];
    QWidget* const parent2 = rows_[1];
    const ui::DateBounds bounds = rules_.bounds();
    const auto make_picker = [&](QWidget* parent) {
        auto* picker = new DatePicker(locale_, parent);
        picker->set_bounds(bounds);
        picker->setFixedWidth(kDateWidth);
        return picker;
    };
    out_from_ = make_picker(parent2);
    out_to_ = make_picker(parent2);
    adults_ = make_numeric(parent2, symbols, 1, kMaxAdults, 1, kAdultsWidth);
    fare_ = new QComboBox(parent2);
    for (const std::string_view item : strings::kFareItems) {
        fare_->addItem(to_qstring(item));
    }
    fare_->setFixedWidth(kFareWidth);
    currency_ = new QComboBox(parent2);
    for (const std::string_view code : strings::kCurrencies) {
        currency_->addItem(to_qstring(code));
    }
    currency_->setFixedWidth(kCurrencyWidth);
    search_out_ = new SearchButton(to_qstring(strings::kSearchOutbound), parent2);
    add_label(row2, strings::kLabelOutboundBetween);
    row2.add_widget(out_from_);
    add_label(row2, strings::kLabelAnd);
    row2.add_widget(out_to_);
    row2.add_gap(kWideGap);
    add_label(row2, strings::kLabelAdults);
    row2.add_widget(adults_);
    row2.add_gap(kSmallGap);
    row2.add_widget(fare_);
    row2.add_gap(kSmallGap);
    add_label(row2, strings::kLabelPricesIn);
    row2.add_widget(currency_);
    row2.add_gap(kButtonGap);
    row2.add_widget(search_out_);

    // Row 3, return.
    FlowLayout& row3 = *row_layouts_[2];
    QWidget* const parent3 = rows_[2];
    ret_from_ = make_picker(parent3);
    ret_to_ = make_picker(parent3);
    min_nights_ = make_numeric(parent3, symbols, 0, ui::DateRules::kMinNightsMaximum, 1, kNightsWidth);
    max_nights_ = make_numeric(parent3, symbols, 0, ui::DateRules::kMaxNightsMaximum, 1, kNightsWidth);
    search_ret_ = new SearchButton(to_qstring(strings::kSearchReturn), parent3);
    add_label(row3, strings::kLabelReturnBetween);
    row3.add_widget(ret_from_);
    add_label(row3, strings::kLabelAnd);
    row3.add_widget(ret_to_);
    row3.add_gap(kWideGap);
    add_label(row3, strings::kLabelRoundTripsStay);
    row3.add_widget(min_nights_);
    add_label(row3, strings::kLabelNightsTo);
    row3.add_widget(max_nights_);
    add_label(row3, strings::kLabelNights);
    row3.add_gap(kButtonGap);
    row3.add_widget(search_ret_);
    add_hint(row3, strings::kHintReturn);

    // The resolved line. It wraps at the panel's client width − 20 (the padding),
    // i.e. MaximumSize.Width = max(200, ClientWidth − 20) for any real window width.
    resolved_ = make_label(strings::kResolvedDefault, this);
    resolved_->setWordWrap(true);
    resolved_->setContentsMargins(kResolvedMargins);
    make_gray(*resolved_);
    outer->addWidget(resolved_);
}

void InputPanel::wire()
{
    const auto wire_picker = [this](DatePicker* picker, DateField field) {
        connect(picker, &QDateEdit::dateChanged, this,
                [this, field](QDate date) { on_picker_changed(field, from_qdate(date)); });
    };
    wire_picker(out_from_, DateField::out_from);
    wire_picker(out_to_, DateField::out_to);
    wire_picker(ret_from_, DateField::ret_from);
    wire_picker(ret_to_, DateField::ret_to);
    connect(min_nights_, &FractionalSpinBox::valueChanged, this, [this](double value) { on_nights_changed(true, value); });
    connect(max_nights_, &FractionalSpinBox::valueChanged, this,
            [this](double value) { on_nights_changed(false, value); });
    connect(currency_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!syncing_) {
            Q_EMIT currency_changed(currency_->currentText());
        }
    });
    connect(search_out_, &QPushButton::clicked, this, [this] {
        if (!working_) {
            Q_EMIT search_outbound_requested();
        }
    });
    connect(search_ret_, &QPushButton::clicked, this, [this] {
        if (!working_) {
            Q_EMIT search_return_requested();
        }
    });

    // Focus tracking for the AcceptButton mapping: the inputs and their inner
    // editors (line edits, calendar) all report FocusIn.
    for (QWidget* widget : tab_order()) {
        widget->installEventFilter(this);
        for (QWidget* child : widget->findChildren<QWidget*>()) {
            child->installEventFilter(this);
        }
    }
}

void InputPanel::apply_tab_order()
{
    const std::vector<QWidget*> order = tab_order();
    for (std::size_t i = 1; i < order.size(); ++i) {
        QWidget::setTabOrder(order[i - 1], order[i]);
    }
}

std::vector<QWidget*> InputPanel::tab_order() const
{
    return {from_,    from_radius_, to_,       to_radius_, out_from_,   out_to_,     adults_,    fare_,
            currency_, search_out_, ret_from_, ret_to_,    min_nights_, max_nights_, search_ret_};
}

std::vector<QWidget*> InputPanel::working_controls() const
{
    return {search_out_, search_ret_, from_,     to_,     from_radius_, to_radius_, out_from_,
            out_to_,     ret_from_,   ret_to_,   adults_, fare_,        currency_};
}

QWidget* InputPanel::row(int index) const
{
    return index >= 0 && index < 3 ? rows_[index] : nullptr;
}

FlowLayout* InputPanel::row_layout(int index) const
{
    return index >= 0 && index < 3 ? row_layouts_[index] : nullptr;
}

// --- Settings ------------------------------------------------------------------------------

void InputPanel::load_from(const ui::AppSettings& settings)
{
    const ui::InitialInputs inputs = ui::initial_inputs(settings);
    const QScopedValueRollback guard(syncing_, true);
    from_->set_text(to_qstring(inputs.from));
    to_->set_text(to_qstring(inputs.to));
    from_radius_->set_value(inputs.from_radius_km);
    to_radius_->set_value(inputs.to_radius_km);
    adults_->set_value(inputs.adults);
    fare_->setCurrentIndex(inputs.fare_index);
    currency_->setCurrentIndex(currency_index(inputs.currency));
    // New nights without running any handler (the rules are "wired after").
    rules_ = ui::DateRules(rules_.bounds(), rules_.out_from(), rules_.out_to(), rules_.ret_from(), rules_.ret_to(),
                           inputs.min_nights, inputs.max_nights);
    min_nights_->set_value(rules_.min_nights());
    max_nights_->set_value(rules_.max_nights());
    sync_pickers();
}

void InputPanel::store_to(ui::AppSettings& settings) const
{
    settings.from = std::string{text::trim(from_text())};
    settings.to = std::string{text::trim(to_text())};
    settings.from_radius_km = from_radius_->int_value();
    settings.to_radius_km = to_radius_->int_value();
    settings.adults = adults_->int_value();
    settings.discount_club = fare_->currentIndex() == 1;
    settings.currency = currency();
    settings.min_nights = min_nights_->int_value();
    settings.max_nights = max_nights_->int_value();
}

void InputPanel::commit_numeric_edits()
{
    for (FractionalSpinBox* box : {from_radius_, to_radius_, adults_, min_nights_, max_nights_}) {
        box->commit();
    }
}

// --- Inputs ---------------------------------------------------------------------------------

std::string InputPanel::from_text() const
{
    return to_utf8(from_->text());
}

std::string InputPanel::to_text() const
{
    return to_utf8(to_->text());
}

double InputPanel::from_radius_km() const
{
    return from_radius_->value();
}

double InputPanel::to_radius_km() const
{
    return to_radius_->value();
}

int InputPanel::adults() const
{
    return adults_->int_value();
}

int InputPanel::fare_index() const
{
    return fare_->currentIndex();
}

PriceType InputPanel::price_type() const
{
    return ui::price_type_for_fare_index(fare_index());
}

std::string InputPanel::currency() const
{
    return to_utf8(currency_->currentText());
}

ui::SearchInputs InputPanel::search_inputs() const
{
    return ui::SearchInputs{from_radius_km(), to_radius_km(), adults(), price_type(), currency()};
}

void InputPanel::set_from_text(std::string_view text)
{
    from_->set_text(to_qstring(text));
}

void InputPanel::set_to_text(std::string_view text)
{
    to_->set_text(to_qstring(text));
}

void InputPanel::set_from_radius_km(double km)
{
    from_radius_->set_value(km);
}

void InputPanel::set_to_radius_km(double km)
{
    to_radius_->set_value(km);
}

void InputPanel::set_adults(double adults)
{
    adults_->set_value(adults);
}

void InputPanel::set_fare_index(int index)
{
    fare_->setCurrentIndex(std::clamp(index, 0, fare_->count() - 1));
}

bool InputPanel::set_currency(std::string_view code)
{
    if (!strings::is_listed_currency(code)) {
        return false;
    }
    currency_->setCurrentIndex(currency_index(code));
    return true;
}

void InputPanel::set_out_from(Date date)
{
    rules_.set_out_from(date);
    sync_pickers();
}

void InputPanel::set_out_to(Date date)
{
    rules_.set_out_to(date);
    sync_pickers();
}

void InputPanel::set_ret_from(Date date)
{
    rules_.set_ret_from(date);
    sync_pickers();
}

void InputPanel::set_ret_to(Date date)
{
    rules_.set_ret_to(date);
    sync_pickers();
}

void InputPanel::set_min_nights(double nights)
{
    min_nights_->set_value(nights);  // → on_nights_changed
}

void InputPanel::set_max_nights(double nights)
{
    max_nights_->set_value(nights);
}

void InputPanel::plan_return_window(Date outbound_date)
{
    rules_.plan_return_window(outbound_date);
    sync_pickers();
}

void InputPanel::apply_autotest_dates(const std::optional<ui::DateWindow>& outbound,
                                      const std::optional<ui::DateWindow>& inbound)
{
    rules_.apply_autotest_dates(outbound, inbound);
    sync_pickers();
}

// --- Map, busy state, resolved line --------------------------------------------------------

void InputPanel::set_suggestions(std::span<const std::string> names)
{
    QStringList list;
    list.reserve(static_cast<qsizetype>(names.size()));
    for (const std::string& name : names) {
        list.push_back(to_qstring(name));
    }
    from_->set_suggestions(list);
    to_->set_suggestions(list);
}

void InputPanel::set_working(bool working)
{
    working_ = working;
    for (QWidget* control : working_controls()) {
        control->setEnabled(!working);
    }
}

void InputPanel::set_resolved_text(std::string_view text)
{
    resolved_->setText(to_qstring(text));
}

QString InputPanel::resolved_text() const
{
    return resolved_->text();
}

// --- Keyboard ------------------------------------------------------------------------------

void InputPanel::focus_from()
{
    from_->setFocus(Qt::OtherFocusReason);
}

InputPanel::FocusGroup InputPanel::focus_group_of(const QWidget* widget) const
{
    for (const QWidget* w = widget; w != nullptr && w != this; w = w->parentWidget()) {
        if (w == from_ || w == to_ || w == from_radius_ || w == to_radius_ || w == out_from_ || w == out_to_ ||
            w == adults_ || w == fare_ || w == currency_) {
            return FocusGroup::outbound;
        }
        if (w == ret_from_ || w == ret_to_) {
            return FocusGroup::inbound;
        }
        if (w == min_nights_ || w == max_nights_) {
            return FocusGroup::nights;
        }
    }
    return FocusGroup::other;
}

bool InputPanel::trigger_accept()
{
    if (working_) {
        return false;
    }
    switch (accept_target_) {
    case AcceptTarget::search_outbound:
        if (search_out_->isEnabled()) {
            Q_EMIT search_outbound_requested();
            return true;
        }
        return false;
    case AcceptTarget::search_return:
        if (search_ret_->isEnabled()) {
            Q_EMIT search_return_requested();
            return true;
        }
        return false;
    case AcceptTarget::none:
        return false;
    }
    return false;
}

bool InputPanel::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::FocusIn) {
        if (const auto* widget = qobject_cast<const QWidget*>(watched)) {
            on_focus_entered(focus_group_of(widget));
        }
    }
    return QWidget::eventFilter(watched, event);
}

void InputPanel::on_focus_entered(FocusGroup group)
{
    switch (group) {
    case FocusGroup::outbound:
        accept_target_ = AcceptTarget::search_outbound;
        break;
    case FocusGroup::inbound:
        accept_target_ = AcceptTarget::search_return;
        break;
    case FocusGroup::nights:
        accept_target_ = AcceptTarget::none;  // Enter only commits (WF-4)
        break;
    case FocusGroup::other:
        break;  // keeps the last mapping
    }
    Q_EMIT focus_group_changed(static_cast<int>(group));
}

// --- Coupling -------------------------------------------------------------------------------

void InputPanel::on_picker_changed(DateField field, Date value)
{
    if (syncing_) {
        return;  // the panel itself is showing the model's values
    }
    switch (field) {
    case DateField::out_from:
        rules_.set_out_from(value);
        break;
    case DateField::out_to:
        rules_.set_out_to(value);
        break;
    case DateField::ret_from:
        rules_.set_ret_from(value);
        break;
    case DateField::ret_to:
        rules_.set_ret_to(value);
        break;
    }
    sync_pickers();
}

void InputPanel::on_nights_changed(bool minimum, double value)
{
    if (syncing_) {
        return;
    }
    const int nights = ui::truncate_toward_zero(value);
    const int recomputes = minimum ? rules_.set_min_nights(nights) : rules_.set_max_nights(nights);
    sync_nights();
    if (recomputes > 0) {
        Q_EMIT nights_changed();
    }
}

void InputPanel::sync_pickers()
{
    const QScopedValueRollback guard(syncing_, true);
    out_from_->set_value(rules_.out_from());
    out_to_->set_value(rules_.out_to());
    ret_from_->set_value(rules_.ret_from());
    ret_to_->set_value(rules_.ret_to());
}

void InputPanel::sync_nights()
{
    // The box the user edited keeps its typed fraction; the other one follows the
    // model when the cascade changed it.
    const QScopedValueRollback guard(syncing_, true);
    if (min_nights_->int_value() != rules_.min_nights()) {
        min_nights_->set_value(rules_.min_nights());
    }
    if (max_nights_->int_value() != rules_.max_nights()) {
        max_nights_->set_value(rules_.max_nights());
    }
}

} // namespace wizz::app
