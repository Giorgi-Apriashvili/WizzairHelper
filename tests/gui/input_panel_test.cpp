// InputPanel: 01 §4.2-§4.5 layout and defaults, §6.1/§6.2 coupling through the
// real widgets (05 U-03), §6.3 keyboard mapping and tab order, §6.4 enable list,
// §13 settings. TODAY = 2026-11-02 (a Monday).

#include "input_panel.hpp"

#include "date_picker.hpp"
#include "flow_layout.hpp"
#include "fractional_spin_box.hpp"
#include "input_test_support.hpp"
#include "place_combo.hpp"
#include "search_button.hpp"
#include "ui_style.hpp"

#include "wizz/core/locale.hpp"
#include "wizz/ui/settings.hpp"
#include "wizz/ui/strings.hpp"

#include <QComboBox>
#include <QFontInfo>
#include <QLabel>
#include <QLineEdit>

#include <gtest/gtest.h>

#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

using wizz::Date;
using wizz::app::DatePicker;
using wizz::app::FractionalSpinBox;
using wizz::app::InputPanel;
using wizz::ui::AppSettings;
using namespace wizz::test::gui;

using FocusGroup = InputPanel::FocusGroup;
using AcceptTarget = InputPanel::AcceptTarget;

[[nodiscard]] constexpr Date nov(unsigned day) noexcept
{
    return Date::from_ymd(2026, 11, day);
}

[[nodiscard]] constexpr Date dec(unsigned day) noexcept
{
    return Date::from_ymd(2026, 12, day);
}

[[nodiscard]] std::unique_ptr<InputPanel> make_panel(const AppSettings& settings = {})
{
    auto panel = std::make_unique<InputPanel>(wizz::Locale::en_us(), kToday, settings);
    panel->setFont(wizz::app::style::form_font());
    return panel;
}

void expect_dates(const InputPanel& panel, Date out_from, Date out_to, Date ret_from, Date ret_to)
{
    EXPECT_EQ(panel.out_from(), out_from);
    EXPECT_EQ(panel.out_to(), out_to);
    EXPECT_EQ(panel.ret_from(), ret_from);
    EXPECT_EQ(panel.ret_to(), ret_to);
    // The widgets always show the model.
    EXPECT_EQ(panel.out_from_picker()->value(), out_from);
    EXPECT_EQ(panel.out_to_picker()->value(), out_to);
    EXPECT_EQ(panel.ret_from_picker()->value(), ret_from);
    EXPECT_EQ(panel.ret_to_picker()->value(), ret_to);
}

/// The texts of a row's labels / the object names of its inputs, left to right ("|gap 24|" for gaps).
[[nodiscard]] std::vector<std::string> row_items(const InputPanel& panel, int row)
{
    std::vector<std::string> items;
    const wizz::app::FlowLayout* layout = panel.row_layout(row);
    for (int i = 0; i < layout->count(); ++i) {
        QLayoutItem* item = layout->itemAt(i);
        if (item->spacerItem() != nullptr) {
            items.push_back("|gap " + std::to_string(item->sizeHint().width()) + "|");
        } else if (const auto* label = qobject_cast<const QLabel*>(item->widget())) {
            items.push_back(label->text().toStdString());
        } else if (item->widget() != nullptr) {
            items.push_back("[" + std::string(item->widget()->metaObject()->className()) + "]");
        }
    }
    return items;
}

[[nodiscard]] QLabel* row_label(const InputPanel& panel, int row, int index)
{
    return qobject_cast<QLabel*>(panel.row_layout(row)->itemAt(index)->widget());
}

/// A user edit of a picker: the widget changes, its dateChanged runs the rules.
void user_sets(DatePicker* picker, Date date)
{
    picker->setDate(wizz::app::to_qdate(date));
}

void user_types(FractionalSpinBox* box, const QString& text)
{
    box->findChild<QLineEdit*>()->selectAll();
    send_key(box, Qt::Key_Delete);
    type_text(box, text);
    send_key(box, Qt::Key_Return);
}

// --- Layout and texts (01 §4.3, §4.4) -----------------------------------------------------

TEST(InputPanel, Rows_contain_the_controls_of_01_4_4_in_order)
{
    const auto panel = make_panel();
    using V = std::vector<std::string>;
    EXPECT_EQ(row_items(*panel, 0),
              (V{"From", "[wizz::app::PlaceCombo]", "within", "[wizz::app::FractionalSpinBox]", "km", "|gap 24|", "To",
                 "[wizz::app::PlaceCombo]", "within", "[wizz::app::FractionalSpinBox]", "km",
                 "(empty “To” = anywhere)"}));
    EXPECT_EQ(row_items(*panel, 1),
              (V{"Outbound between", "[wizz::app::DatePicker]", "and", "[wizz::app::DatePicker]", "|gap 24|", "Adults",
                 "[wizz::app::FractionalSpinBox]", "|gap 8|", "[QComboBox]", "|gap 8|", "Prices in", "[QComboBox]",
                 "|gap 16|", "[wizz::app::SearchButton]"}));
    EXPECT_EQ(row_items(*panel, 2),
              (V{"Return between", "[wizz::app::DatePicker]", "and", "[wizz::app::DatePicker]", "|gap 24|",
                 "Round trips: stay", "[wizz::app::FractionalSpinBox]", "to", "[wizz::app::FractionalSpinBox]",
                 "nights", "|gap 16|", "[wizz::app::SearchButton]",
                 "Return flights go from the destination area back to any airport near home."}));
    EXPECT_EQ(panel->row_layout(0)->itemAt(1)->widget(), panel->from_box());
    EXPECT_EQ(panel->row_layout(0)->itemAt(7)->widget(), panel->to_box());
    EXPECT_EQ(panel->row_layout(1)->itemAt(13)->widget(), panel->search_outbound_button());
    EXPECT_EQ(panel->row_layout(2)->itemAt(11)->widget(), panel->search_return_button());
    EXPECT_EQ(panel->row(3), nullptr);
}

TEST(InputPanel, Labels_hints_gaps_and_paddings_use_the_01_4_3_margins)
{
    const auto panel = make_panel();
    const wizz::app::FlowLayout* row1 = panel->row_layout(0);
    EXPECT_EQ(row1->item_margins(0), QMargins(3, 7, 3, 0));    // Lbl("From")
    EXPECT_EQ(row1->item_margins(1), QMargins(3, 3, 3, 3));    // _from: WinForms default margin
    EXPECT_EQ(row1->item_margins(5), QMargins());              // Gap(24)
    EXPECT_EQ(row1->item_margins(11), QMargins(10, 7, 3, 0));  // Hint(...)
    EXPECT_EQ(row1->contentsMargins(), QMargins(0, 1, 0, 1));
    EXPECT_EQ(panel->layout()->contentsMargins(), QMargins(10, 8, 10, 4));

    const QColor gray = wizz::app::style::gray_text();
    EXPECT_EQ(row_label(*panel, 0, 11)->palette().color(QPalette::WindowText), gray);
    EXPECT_EQ(row_label(*panel, 2, 12)->palette().color(QPalette::WindowText), gray);
    EXPECT_NE(row_label(*panel, 0, 0)->palette().color(QPalette::WindowText), gray);

    QLabel* resolved = panel->resolved_label();
    EXPECT_EQ(resolved->text(),
              QStringLiteral("Fares come from Wizz Air's website. Double-click a result to open it on wizzair.com."));
    EXPECT_TRUE(resolved->wordWrap());
    EXPECT_EQ(resolved->contentsMargins(), QMargins(3, 4, 3, 2));
    EXPECT_EQ(resolved->palette().color(QPalette::WindowText), gray);
    EXPECT_EQ(resolved->textFormat(), Qt::PlainText);
    panel->set_resolved_text("Outbound: from Budapest (BUD), Hungary → airports within 250 km: BUD 0 km.   To: anywhere.");
    EXPECT_EQ(panel->resolved_text().toStdString(),
              "Outbound: from Budapest (BUD), Hungary → airports within 250 km: BUD 0 km.   To: anywhere.");
}

TEST(InputPanel, Widths_ranges_steps_and_tooltips_follow_the_table)
{
    const auto panel = make_panel();
    EXPECT_EQ(panel->from_box()->width(), 210);
    EXPECT_EQ(panel->to_box()->width(), 210);
    EXPECT_EQ(panel->from_radius_box()->width(), 64);
    EXPECT_EQ(panel->to_radius_box()->width(), 64);
    for (DatePicker* picker :
         {panel->out_from_picker(), panel->out_to_picker(), panel->ret_from_picker(), panel->ret_to_picker()}) {
        EXPECT_EQ(picker->width(), 130);
        EXPECT_EQ(picker->bounds().min, kToday);
        EXPECT_EQ(picker->bounds().max, Date::from_ymd(2028, 11, 2));
        EXPECT_EQ(picker->displayFormat(), QStringLiteral("ddd dd MMM yyyy"));
    }
    EXPECT_EQ(panel->adults_box()->width(), 48);
    EXPECT_EQ(panel->fare_box()->width(), 175);
    EXPECT_EQ(panel->currency_box()->width(), 70);
    EXPECT_EQ(panel->min_nights_box()->width(), 52);
    EXPECT_EQ(panel->max_nights_box()->width(), 52);

    const auto expect_numeric = [](const FractionalSpinBox* box, double minimum, double maximum, double step) {
        EXPECT_EQ(box->minimum(), minimum);
        EXPECT_EQ(box->maximum(), maximum);
        EXPECT_EQ(box->single_step(), step);
    };
    expect_numeric(panel->from_radius_box(), 0, 1500, 25);
    expect_numeric(panel->to_radius_box(), 0, 1500, 25);
    expect_numeric(panel->adults_box(), 1, 9, 1);
    expect_numeric(panel->min_nights_box(), 0, 60, 1);
    expect_numeric(panel->max_nights_box(), 0, 120, 1);

    EXPECT_EQ(panel->from_box()->toolTip().toStdString(),
              "City or airport you start from (any city works — nearby Wizz Air airports are found automatically)");
    EXPECT_EQ(panel->to_box()->toolTip().toStdString(),
              "Destination city or airport. Leave empty to search ANYWHERE Wizz Air flies.");
    EXPECT_EQ(panel->search_outbound_button()->text(), QStringLiteral("Search outbound"));
    EXPECT_EQ(panel->search_return_button()->text(), QStringLiteral("Search return"));
    EXPECT_FALSE(panel->fare_box()->isEditable());
    EXPECT_FALSE(panel->currency_box()->isEditable());
}

TEST(InputPanel, Currency_list_has_the_21_codes_in_order_and_fare_items)
{
    const auto panel = make_panel();
    QStringList codes;
    for (int i = 0; i < panel->currency_box()->count(); ++i) {
        codes.push_back(panel->currency_box()->itemText(i));
    }
    EXPECT_EQ(codes.join(u','), QStringLiteral("EUR,GBP,USD,HUF,PLN,RON,CZK,CHF,SEK,NOK,DKK,GEL,RSD,MKD,ALL,BAM,MDL,"
                                               "AED,UAH,TRY,ILS"));
    ASSERT_EQ(panel->fare_box()->count(), 2);
    EXPECT_EQ(panel->fare_box()->itemText(0), QStringLiteral("Regular fares"));
    EXPECT_EQ(panel->fare_box()->itemText(1), QStringLiteral("Wizz Discount Club fares"));
}

// --- Defaults and settings (01 §4.4, §13) -------------------------------------------------

TEST(InputPanel, Defaults_from_default_settings)
{
    const auto panel = make_panel();
    EXPECT_EQ(panel->from_text(), "Budapest");
    EXPECT_EQ(panel->to_text(), "London");
    EXPECT_EQ(panel->from_radius_km(), 250.0);
    EXPECT_EQ(panel->to_radius_km(), 100.0);
    EXPECT_EQ(panel->adults(), 1);
    EXPECT_EQ(panel->fare_index(), 0);
    EXPECT_EQ(panel->price_type(), wizz::PriceType::regular);
    EXPECT_EQ(panel->currency(), "EUR");
    EXPECT_EQ(panel->min_nights(), 2);
    EXPECT_EQ(panel->max_nights(), 10);
    // today + 7 / + 37 / + 10 / + 44: the rules are wired after the defaults.
    expect_dates(*panel, nov(9), dec(9), nov(12), dec(16));
    EXPECT_EQ(panel->out_from_picker()->text(), QStringLiteral("Mon 09 Nov 2026"));
    EXPECT_EQ(panel->ret_to_picker()->text(), QStringLiteral("Wed 16 Dec 2026"));
    EXPECT_EQ(panel->from_radius_box()->text(), QStringLiteral("250"));
    EXPECT_EQ(panel->min_nights_box()->text(), QStringLiteral("2"));
    EXPECT_EQ(panel->max_nights_box()->text(), QStringLiteral("10"));
    EXPECT_EQ(panel->accept_target(), AcceptTarget::search_outbound);
    EXPECT_FALSE(panel->is_working());

    const wizz::ui::SearchInputs inputs = panel->search_inputs();
    EXPECT_EQ(inputs.from_radius_km, 250.0);
    EXPECT_EQ(inputs.to_radius_km, 100.0);
    EXPECT_EQ(inputs.adults, 1);
    EXPECT_EQ(inputs.currency, "EUR");
}

TEST(InputPanel, Defaults_from_a_settings_file_are_clamped)
{
    const AppSettings settings = AppSettings::parse(R"({
  "From": "Wien",
  "To": "",
  "FromRadiusKm": 2000,
  "ToRadiusKm": -5,
  "Adults": 12,
  "DiscountClub": true,
  "Currency": "eur",
  "MinNights": 70,
  "MaxNights": 30,
  "WindowWidth": 1500,
  "WindowHeight": 900,
  "Maximized": true
})");
    ASSERT_EQ(settings.from, "Wien");  // the JSON is valid: no fallback to defaults
    const auto panel = make_panel(settings);
    EXPECT_EQ(panel->from_text(), "Wien");
    EXPECT_EQ(panel->to_text(), "");                  // empty = anywhere
    EXPECT_EQ(panel->from_radius_km(), 1500.0);       // clamp 0..1500
    EXPECT_EQ(panel->to_radius_km(), 0.0);
    EXPECT_EQ(panel->adults(), 9);                    // clamp 1..9
    EXPECT_EQ(panel->fare_index(), 1);                // DiscountClub
    EXPECT_EQ(panel->price_type(), wizz::PriceType::wizz_discount_club);
    EXPECT_EQ(panel->currency(), "EUR");              // "eur" is not listed (case-sensitive)
    EXPECT_EQ(panel->min_nights(), 60);               // clamp 0..60
    EXPECT_EQ(panel->max_nights(), 60);               // clamp 0..120, then ≥ MinNights
    EXPECT_EQ(panel->max_nights_box()->value(), 60.0);
    expect_dates(*panel, nov(9), dec(9), nov(12), dec(16));  // dates are not persisted

    AppSettings other;
    other.currency = "HUF";
    other.min_nights = 5;
    other.max_nights = 3;
    other.adults = 0;
    panel->load_from(other);
    EXPECT_EQ(panel->currency(), "HUF");
    EXPECT_EQ(panel->min_nights(), 5);
    EXPECT_EQ(panel->max_nights(), 5);
    EXPECT_EQ(panel->adults(), 1);
    EXPECT_EQ(panel->from_text(), "Budapest");
    EXPECT_EQ(panel->to_text(), "London");

    other.currency = "XYZ";
    panel->load_from(other);
    EXPECT_EQ(panel->currency(), "EUR");
}

TEST(InputPanel, Load_from_runs_no_handler_and_emits_nothing)
{
    const auto panel = make_panel();
    int currency_signals = 0;
    int nights_signals = 0;
    QObject::connect(panel.get(), &InputPanel::currency_changed, [&] { ++currency_signals; });
    QObject::connect(panel.get(), &InputPanel::nights_changed, [&] { ++nights_signals; });
    panel->set_out_from(nov(20));  // move the dates first
    const Date ret_from = panel->ret_from();
    AppSettings settings;
    settings.currency = "GBP";
    settings.min_nights = 30;
    settings.max_nights = 40;
    panel->load_from(settings);
    EXPECT_EQ(currency_signals, 0);
    EXPECT_EQ(nights_signals, 0);
    EXPECT_EQ(panel->currency(), "GBP");
    EXPECT_EQ(panel->min_nights(), 30);
    EXPECT_EQ(panel->ret_from(), ret_from);  // nights move no dates
    EXPECT_EQ(panel->out_from(), nov(20));
}

TEST(InputPanel, Store_to_trims_texts_and_truncates_integers)
{
    const auto panel = make_panel();
    panel->set_from_text("  Debrecen \t");
    panel->set_to_text(" ");
    user_types(panel->from_radius_box(), QStringLiteral("250.7"));
    user_types(panel->to_radius_box(), QStringLiteral("99.9"));
    user_types(panel->adults_box(), QStringLiteral("2.7"));
    panel->set_fare_index(1);
    ASSERT_TRUE(panel->set_currency("PLN"));
    panel->set_max_nights(14.9);
    panel->set_min_nights(3.5);

    EXPECT_EQ(panel->from_radius_km(), 250.7);  // radii keep the fraction for searches
    EXPECT_EQ(panel->to_radius_km(), 99.9);
    EXPECT_EQ(panel->adults_box()->text(), QStringLiteral("3"));
    EXPECT_EQ(panel->adults(), 2);  // searches with 2

    AppSettings settings;
    settings.window_width = 1234;
    settings.window_height = 777;
    settings.maximized = true;
    panel->store_to(settings);
    EXPECT_EQ(settings.from, "Debrecen");
    EXPECT_EQ(settings.to, "");  // trimmed; empty is saved
    EXPECT_EQ(settings.from_radius_km, 250);
    EXPECT_EQ(settings.to_radius_km, 99);
    EXPECT_EQ(settings.adults, 2);
    EXPECT_TRUE(settings.discount_club);
    EXPECT_EQ(settings.currency, "PLN");
    EXPECT_EQ(settings.min_nights, 3);
    EXPECT_EQ(settings.max_nights, 14);
    EXPECT_EQ(settings.window_width, 1234);  // window fields are the main window's
    EXPECT_EQ(settings.window_height, 777);
    EXPECT_TRUE(settings.maximized);
}

// --- Date coupling through the widgets (01 §6.1, 05 U-03) --------------------------------

TEST(InputPanel, U_03_Moving_the_outbound_start_moves_the_return_window)
{
    const auto panel = make_panel();
    user_sets(panel->out_from_picker(), nov(12));  // +3 days
    expect_dates(*panel, nov(12), dec(9), nov(15), dec(19));
    EXPECT_EQ(days_between(panel->ret_to(), panel->ret_from()), 34);  // length kept
    user_sets(panel->out_from_picker(), nov(5));  // −7 days
    expect_dates(*panel, nov(5), dec(9), nov(8), dec(12));
    EXPECT_EQ(panel->ret_from_picker()->text(), QStringLiteral("Sun 08 Nov 2026"));
}

TEST(InputPanel, U_03_Clamping_can_shorten_the_return_window)
{
    const auto panel = make_panel();
    user_sets(panel->out_from_picker(), Date::from_ymd(2028, 10, 30));
    expect_dates(*panel, Date::from_ymd(2028, 10, 30), Date::from_ymd(2028, 10, 30), Date::from_ymd(2028, 11, 2),
                 Date::from_ymd(2028, 11, 2));

    const auto low = make_panel();
    user_sets(low->ret_from_picker(), nov(3));
    user_sets(low->out_from_picker(), nov(2));  // −7: return start clamped to today, length kept from there
    expect_dates(*low, nov(2), dec(9), nov(2), dec(15));
}

TEST(InputPanel, A_later_outbound_end_extends_the_return_end_without_moving_its_start)
{
    const auto panel = make_panel();
    user_sets(panel->out_to_picker(), dec(20));  // retTo ≥ outTo + minNights (LIVE-4)
    expect_dates(*panel, nov(9), dec(20), nov(12), dec(22));
    user_sets(panel->out_to_picker(), dec(10));  // earlier: nothing shrinks
    expect_dates(*panel, nov(9), dec(10), nov(12), dec(22));

    panel->set_min_nights(10);  // nights feed the rule but move no dates
    expect_dates(*panel, nov(9), dec(10), nov(12), dec(22));
    user_sets(panel->out_to_picker(), dec(15));
    expect_dates(*panel, nov(9), dec(15), nov(12), dec(25));
}

TEST(InputPanel, Guarded_cases_do_not_apply_the_other_rule)
{
    // outTo pulls outFrom back: no return-window shift.
    const auto panel = make_panel();
    user_sets(panel->out_to_picker(), nov(5));
    expect_dates(*panel, nov(5), nov(5), nov(12), dec(16));
    EXPECT_EQ(panel->date_rules().last_out_from(), nov(5));
    user_sets(panel->out_from_picker(), nov(6));  // +1 from the updated _lastOutFrom
    expect_dates(*panel, nov(6), nov(6), nov(13), dec(17));

    // outFrom pushes outTo: the "return end ≥ outTo + minNights" rule is skipped.
    const auto other = make_panel();
    user_sets(other->ret_from_picker(), nov(2));
    user_sets(other->ret_to_picker(), nov(2));
    expect_dates(*other, nov(9), dec(9), nov(2), nov(2));
    user_sets(other->out_from_picker(), dec(12));
    expect_dates(*other, dec(12), dec(12), dec(5), dec(5));
}

TEST(InputPanel, Return_handlers_keep_from_before_to)
{
    const auto panel = make_panel();
    user_sets(panel->ret_from_picker(), dec(20));
    EXPECT_EQ(panel->ret_to(), dec(20));
    user_sets(panel->ret_to_picker(), dec(1));
    expect_dates(*panel, nov(9), dec(9), dec(1), dec(1));
}

TEST(InputPanel, Programmatic_dates_are_clamped_and_run_the_rules)
{
    const auto panel = make_panel();
    panel->set_ret_to(Date::from_ymd(2030, 1, 1));
    EXPECT_EQ(panel->ret_to(), Date::from_ymd(2028, 11, 2));
    panel->set_out_to(Date::from_ymd(2020, 1, 1));  // clamped to today; outFrom pulled back
    EXPECT_EQ(panel->out_to(), kToday);
    EXPECT_EQ(panel->out_from(), kToday);
    EXPECT_EQ(panel->out_from_picker()->value(), kToday);

    const auto other = make_panel();
    other->set_out_from(nov(12));  // a code change runs the handlers like a user change
    expect_dates(*other, nov(12), dec(9), nov(15), dec(19));
    user_sets(other->ret_to_picker(), Date::from_ymd(2020, 1, 1));  // Qt clamps the user value too
    expect_dates(*other, nov(12), dec(9), kToday, kToday);
}

TEST(InputPanel, Right_click_return_window_and_autotest_dates)
{
    const auto panel = make_panel();
    panel->plan_return_window(nov(20));
    expect_dates(*panel, nov(9), dec(9), nov(22), nov(30));  // 11-20 + [2, 10] nights
    panel->apply_autotest_dates(wizz::ui::DateWindow{dec(1), dec(10)}, wizz::ui::DateWindow{dec(5), dec(20)});
    expect_dates(*panel, dec(1), dec(10), dec(5), dec(20));
}

TEST(InputPanel, U_03_From_never_exceeds_to_and_everything_stays_in_range)
{
    const auto panel = make_panel();
    std::mt19937 random{20261102};
    std::uniform_int_distribution<int> field(0, 5);
    std::uniform_int_distribution<int> offset(-30, 800);
    std::uniform_int_distribution<int> nights(-5, 130);
    const Date max = Date::from_ymd(2028, 11, 2);
    for (int step = 0; step < 400; ++step) {
        const Date value = kToday.add_days(offset(random));
        switch (field(random)) {
        case 0:
            user_sets(panel->out_from_picker(), value);
            break;
        case 1:
            user_sets(panel->out_to_picker(), value);
            break;
        case 2:
            user_sets(panel->ret_from_picker(), value);
            break;
        case 3:
            user_sets(panel->ret_to_picker(), value);
            break;
        case 4:
            panel->set_min_nights(nights(random));
            break;
        default:
            panel->set_max_nights(nights(random));
            break;
        }
        ASSERT_LE(panel->out_from(), panel->out_to()) << step;
        ASSERT_LE(panel->ret_from(), panel->ret_to()) << step;
        ASSERT_LE(panel->min_nights(), panel->max_nights()) << step;
        for (DatePicker* picker : {panel->out_from_picker(), panel->out_to_picker(), panel->ret_from_picker(),
                                   panel->ret_to_picker()}) {
            ASSERT_GE(picker->value(), kToday) << step;
            ASSERT_LE(picker->value(), max) << step;
        }
        ASSERT_EQ(panel->out_from_picker()->value(), panel->out_from()) << step;
        ASSERT_EQ(panel->ret_to_picker()->value(), panel->ret_to()) << step;
    }
}

// --- Nights coupling (01 §6.2) ------------------------------------------------------------

TEST(InputPanel, Nights_coupling_keeps_min_at_most_max_and_reports_changes)
{
    const auto panel = make_panel();
    int recomputes = 0;
    QObject::connect(panel.get(), &InputPanel::nights_changed, [&] { ++recomputes; });

    user_types(panel->min_nights_box(), QStringLiteral("15"));  // max follows up
    EXPECT_EQ(panel->min_nights(), 15);
    EXPECT_EQ(panel->max_nights(), 15);
    EXPECT_EQ(panel->max_nights_box()->text(), QStringLiteral("15"));
    EXPECT_EQ(recomputes, 1);

    user_types(panel->max_nights_box(), QStringLiteral("3"));  // min follows down
    EXPECT_EQ(panel->min_nights(), 3);
    EXPECT_EQ(panel->min_nights_box()->text(), QStringLiteral("3"));
    EXPECT_EQ(recomputes, 2);

    user_types(panel->max_nights_box(), QStringLiteral("100"));
    EXPECT_EQ(panel->min_nights(), 3);
    EXPECT_EQ(panel->max_nights(), 100);
    EXPECT_EQ(recomputes, 3);

    user_types(panel->min_nights_box(), QStringLiteral("3.7"));  // truncated: the filter stays 3
    EXPECT_EQ(panel->min_nights_box()->text(), QStringLiteral("4"));
    EXPECT_EQ(panel->min_nights(), 3);
    EXPECT_EQ(recomputes, 3);

    user_types(panel->min_nights_box(), QStringLiteral("99"));  // clamped to 60
    EXPECT_EQ(panel->min_nights(), 60);
    EXPECT_EQ(panel->min_nights_box()->value(), 60.0);
    EXPECT_EQ(recomputes, 4);

    expect_dates(*panel, nov(9), dec(9), nov(12), dec(16));  // nights move no dates
}

// --- Busy state (01 §6.4) and signals ------------------------------------------------------

TEST(InputPanel, Set_working_disables_exactly_the_01_6_4_controls)
{
    const auto panel = make_panel();
    const std::vector<QWidget*> expected{panel->search_outbound_button(),
                                         panel->search_return_button(),
                                         panel->from_box(),
                                         panel->to_box(),
                                         panel->from_radius_box(),
                                         panel->to_radius_box(),
                                         panel->out_from_picker(),
                                         panel->out_to_picker(),
                                         panel->ret_from_picker(),
                                         panel->ret_to_picker(),
                                         panel->adults_box(),
                                         panel->fare_box(),
                                         panel->currency_box()};
    EXPECT_EQ(panel->working_controls(), expected);

    panel->set_working(true);
    EXPECT_TRUE(panel->is_working());
    for (QWidget* control : expected) {
        EXPECT_FALSE(control->isEnabled()) << control->metaObject()->className();
    }
    EXPECT_TRUE(panel->min_nights_box()->isEnabled());  // nights stay enabled
    EXPECT_TRUE(panel->max_nights_box()->isEnabled());
    EXPECT_TRUE(panel->isEnabled());

    panel->set_working(false);
    for (QWidget* control : expected) {
        EXPECT_TRUE(control->isEnabled());
    }
}

TEST(InputPanel, Search_buttons_emit_their_requests_unless_working)
{
    const auto panel = make_panel();
    int outbound = 0;
    int inbound = 0;
    QObject::connect(panel.get(), &InputPanel::search_outbound_requested, [&] { ++outbound; });
    QObject::connect(panel.get(), &InputPanel::search_return_requested, [&] { ++inbound; });
    panel->search_outbound_button()->click();
    panel->search_return_button()->click();
    EXPECT_EQ(outbound, 1);
    EXPECT_EQ(inbound, 1);
    panel->set_working(true);
    panel->search_outbound_button()->click();
    EXPECT_FALSE(panel->trigger_accept());
    EXPECT_EQ(outbound, 1);
}

TEST(InputPanel, Currency_changes_are_signalled_and_only_listed_codes_are_accepted)
{
    const auto panel = make_panel();
    QStringList changes;
    QObject::connect(panel.get(), &InputPanel::currency_changed, [&](const QString& code) { changes.push_back(code); });
    EXPECT_TRUE(panel->set_currency("HUF"));
    EXPECT_FALSE(panel->set_currency("eur"));
    EXPECT_FALSE(panel->set_currency("XYZ"));
    EXPECT_TRUE(panel->set_currency("HUF"));  // unchanged: no signal
    panel->currency_box()->setCurrentIndex(20);  // the user picks ILS
    EXPECT_EQ(changes, (QStringList{QStringLiteral("HUF"), QStringLiteral("ILS")}));
    EXPECT_EQ(panel->currency(), "ILS");
}

TEST(InputPanel, Suggestions_reach_both_place_boxes_and_keep_the_typed_texts)
{
    const auto panel = make_panel();
    panel->set_to_text("");
    const std::vector<std::string> names{"Budapest", "Kraków", "London"};
    panel->set_suggestions(names);
    EXPECT_EQ(panel->from_box()->suggestions(),
              (QStringList{QStringLiteral("Budapest"), QString::fromUtf8("Kraków"), QStringLiteral("London")}));
    EXPECT_EQ(panel->to_box()->count(), 3);
    EXPECT_EQ(panel->from_text(), "Budapest");
    EXPECT_EQ(panel->to_text(), "");
}

// --- Keyboard (01 §6.3) -----------------------------------------------------------------------

TEST(InputPanel, Accept_button_follows_focus)
{
    const auto panel = make_panel();
    std::vector<int> groups;
    QObject::connect(panel.get(), &InputPanel::focus_group_changed, [&](int group) { groups.push_back(group); });
    int outbound = 0;
    int inbound = 0;
    QObject::connect(panel.get(), &InputPanel::search_outbound_requested, [&] { ++outbound; });
    QObject::connect(panel.get(), &InputPanel::search_return_requested, [&] { ++inbound; });

    EXPECT_EQ(panel->accept_target(), AcceptTarget::search_outbound);  // initial mapping

    send_focus_in(panel->ret_to_picker());
    EXPECT_EQ(panel->accept_target(), AcceptTarget::search_return);
    EXPECT_TRUE(panel->trigger_accept());
    EXPECT_EQ(inbound, 1);

    send_focus_in(panel->search_outbound_button());  // anything else keeps the mapping
    EXPECT_EQ(panel->accept_target(), AcceptTarget::search_return);

    send_focus_in(panel->max_nights_box());  // nights: Enter only commits (WF-4)
    EXPECT_EQ(panel->accept_target(), AcceptTarget::none);
    EXPECT_FALSE(panel->trigger_accept());

    send_focus_in(panel->currency_box());
    EXPECT_EQ(panel->accept_target(), AcceptTarget::search_outbound);
    EXPECT_TRUE(panel->trigger_accept());
    EXPECT_EQ(outbound, 1);

    send_focus_in(panel->from_box()->lineEdit());  // inner editors count for their box
    EXPECT_EQ(panel->accept_target(), AcceptTarget::search_outbound);

    EXPECT_EQ(groups, (std::vector<int>{static_cast<int>(FocusGroup::inbound), static_cast<int>(FocusGroup::other),
                                        static_cast<int>(FocusGroup::nights), static_cast<int>(FocusGroup::outbound),
                                        static_cast<int>(FocusGroup::outbound)}));

    for (QWidget* widget : {static_cast<QWidget*>(panel->from_box()), static_cast<QWidget*>(panel->to_box()),
                            static_cast<QWidget*>(panel->from_radius_box()),
                            static_cast<QWidget*>(panel->to_radius_box()),
                            static_cast<QWidget*>(panel->out_from_picker()),
                            static_cast<QWidget*>(panel->out_to_picker()), static_cast<QWidget*>(panel->adults_box()),
                            static_cast<QWidget*>(panel->fare_box()), static_cast<QWidget*>(panel->currency_box())}) {
        EXPECT_EQ(panel->focus_group_of(widget), FocusGroup::outbound);
    }
    EXPECT_EQ(panel->focus_group_of(panel->ret_from_picker()), FocusGroup::inbound);
    EXPECT_EQ(panel->focus_group_of(panel->min_nights_box()), FocusGroup::nights);
    EXPECT_EQ(panel->focus_group_of(panel->search_return_button()), FocusGroup::other);
    EXPECT_EQ(panel->focus_group_of(panel.get()), FocusGroup::other);

    // A disabled target button is not pressed (PerformClick on a disabled button).
    panel->search_outbound_button()->setEnabled(false);
    EXPECT_FALSE(panel->trigger_accept());
}

TEST(InputPanel, Enter_in_an_input_commits_and_is_left_for_the_accept_button)
{
    const auto panel = make_panel();
    FractionalSpinBox* radius = panel->from_radius_box();
    radius->findChild<QLineEdit*>()->selectAll();
    type_text(radius, QStringLiteral("300"));
    EXPECT_FALSE(send_key(radius, Qt::Key_Return));  // not consumed: the window presses AcceptButton
    EXPECT_EQ(panel->from_radius_km(), 300.0);
    EXPECT_FALSE(send_key(panel->out_from_picker(), Qt::Key_Return));
    EXPECT_FALSE(send_key(panel->from_box(), Qt::Key_Return));
}

TEST(InputPanel, Tab_order_follows_01_6_3)
{
    const auto panel = make_panel();
    const std::vector<QWidget*> expected = panel->tab_order();
    ASSERT_EQ(expected.size(), 15u);
    EXPECT_EQ(expected.front(), panel->from_box());
    EXPECT_EQ(expected[9], panel->search_outbound_button());
    EXPECT_EQ(expected.back(), panel->search_return_button());

    // Walk the focus chain from "From" and keep the listed widgets in visiting order.
    std::vector<QWidget*> visited;
    QWidget* widget = panel->from_box();
    for (int guard = 0; guard < 500 && visited.size() < expected.size(); ++guard) {
        if (std::ranges::find(expected, widget) != expected.end() &&
            std::ranges::find(visited, widget) == visited.end()) {
            visited.push_back(widget);
        }
        widget = widget->nextInFocusChain();
    }
    EXPECT_EQ(visited, expected);

    // Labels and gaps never take focus.
    for (int row = 0; row < 3; ++row) {
        const wizz::app::FlowLayout* layout = panel->row_layout(row);
        for (int i = 0; i < layout->count(); ++i) {
            if (const auto* label = qobject_cast<const QLabel*>(layout->itemAt(i)->widget())) {
                EXPECT_EQ(label->focusPolicy(), Qt::NoFocus);
            }
        }
    }
}

// --- Wrapping rows (01 §4.3) ------------------------------------------------------------------

TEST(InputPanel, Rows_wrap_in_narrow_windows)
{
    // A child of a host window, like in the main window (a top-level panel would get
    // its layout's minimum width enforced).
    QWidget host;
    host.setAttribute(Qt::WA_DontShowOnScreen);
    host.resize(1500, 600);
    auto* panel = new InputPanel(wizz::Locale::en_us(), kToday, {}, &host);
    panel->setFont(wizz::app::style::form_font());
    host.show();
    // Real Segoe UI metrics (a manual run on the windows platform) or the minimal
    // platform's box glyphs.
    const bool real_fonts = QFontInfo(panel->font()).family() == QStringLiteral("Segoe UI");

    const auto place = [&](int width) {
        panel->setGeometry(0, 0, width, panel->heightForWidth(width));
        panel->layout()->activate();
    };
    const auto expect_wrapping_matches_the_width = [&] {
        for (int row = 0; row < 3; ++row) {
            const wizz::app::FlowLayout* layout = panel->row_layout(row);
            const int width = panel->row(row)->width();
            EXPECT_EQ(layout->line_count(width) == 1, layout->sizeHint().width() <= width) << row;
            EXPECT_EQ(panel->row(row)->height(), layout->heightForWidth(width)) << row;
        }
    };

    const int wide_height = panel->heightForWidth(1400);
    const int narrow_height = panel->heightForWidth(600);
    EXPECT_GT(narrow_height, wide_height);

    place(1400);
    EXPECT_EQ(panel->row(0)->width(), 1400 - 20);  // padding 10 left and right
    expect_wrapping_matches_the_width();
    if (real_fonts) {
        for (int row = 0; row < 3; ++row) {
            EXPECT_EQ(panel->row_layout(row)->line_count(panel->row(row)->width()), 1) << row;
        }
    }
    // Rows 1 and 2 fit on one line: every input shares the line top (+ its 3 px margin).
    EXPECT_EQ(panel->from_box()->y(), panel->to_radius_box()->y());
    EXPECT_EQ(panel->from_box()->y(), 1 + 3);
    EXPECT_EQ(row_label(*panel, 0, 0)->y(), 1 + 7);
    EXPECT_EQ(panel->out_from_picker()->y(), panel->currency_box()->y());
    EXPECT_EQ(panel->out_from_picker()->y(), panel->search_outbound_button()->y());
    EXPECT_EQ(panel->row(0)->y(), 8);  // padding top
    save_render(*panel, QStringLiteral("input-panel-1400"));

    place(600);
    expect_wrapping_matches_the_width();
    EXPECT_GT(panel->row_layout(0)->line_count(panel->row(0)->width()), 1);
    EXPECT_GT(panel->row_layout(1)->line_count(panel->row(1)->width()), 1);
    EXPECT_GT(panel->to_box()->y(), panel->from_box()->y());  // "To" moved to a second line
    EXPECT_GT(panel->search_outbound_button()->y(), panel->out_from_picker()->y());
    for (int row = 0; row < 3; ++row) {
        const QRect row_rect = panel->row(row)->rect();
        for (QWidget* child : panel->row(row)->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
            EXPECT_TRUE(row_rect.contains(child->geometry()) || child->width() > row_rect.width())
                << child->metaObject()->className();
        }
    }
    EXPECT_GT(panel->resolved_label()->y(), panel->row(2)->geometry().bottom());
    save_render(*panel, QStringLiteral("input-panel-600"));

    panel->set_working(true);
    place(1400);
    save_render(*panel, QStringLiteral("input-panel-1400-working"));
}
} // namespace
