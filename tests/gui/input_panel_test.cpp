// InputPanel: the 01 §4.4 controls and defaults in the cards of DECISIONS D-53,
// §6.1/§6.2 coupling through the real widgets (05 U-03), §6.3 keyboard mapping and
// the Tab order of D-54, §6.4 enable list, §13 settings. TODAY = 2026-11-02 (a Monday).

#include "input_panel.hpp"

#include "date_picker.hpp"
#include "fractional_spin_box.hpp"
#include "input_test_support.hpp"
#include "minimum_window.hpp"
#include "modern_style.hpp"
#include "place_combo.hpp"
#include "search_button.hpp"
#include "section_card.hpp"
#include "ui_style.hpp"

#include "wizz/core/locale.hpp"
#include "wizz/ui/settings.hpp"
#include "wizz/ui/strings.hpp"

#include <QComboBox>
#include <QFontInfo>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <utility>
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

/// A card's caption row and body in layout order: label texts, "(icon)" for the accent
/// glyph and "[class]" for the inputs (spacing and stretch left out).
[[nodiscard]] std::vector<std::string> card_items(const QWidget* card)
{
    std::vector<std::string> items;
    const std::function<void(const QLayout*)> walk = [&](const QLayout* layout) {
        for (int i = 0; i < layout->count(); ++i) {
            QLayoutItem* item = layout->itemAt(i);
            if (const QLayout* inner = item->layout()) {
                walk(inner);
            } else if (const auto* label = qobject_cast<const QLabel*>(item->widget())) {
                items.push_back(label->text().toStdString());
            } else if (qobject_cast<const wizz::app::GlyphLabel*>(item->widget()) != nullptr) {
                items.emplace_back("(icon)");
            } else if (item->widget() != nullptr) {
                items.push_back("[" + std::string(item->widget()->metaObject()->className()) + "]");
            }
        }
    };
    walk(card->layout());
    return items;
}

[[nodiscard]] const wizz::app::SectionCard* card(const InputPanel& panel, int index)
{
    return qobject_cast<const wizz::app::SectionCard*>(panel.row(index));
}

/// The panel as a child of a shown host window of `width` (like in the main window: a
/// top-level panel would get its layout's minimum width enforced), laid out.
class HostedPanel {
public:
    explicit HostedPanel(int width)
    {
        host_.setAttribute(Qt::WA_DontShowOnScreen);
        panel_ = new InputPanel(wizz::Locale::en_us(), kToday, {}, &host_);
        host_.show();
        place(width);
    }
    void place(int width)
    {
        host_.resize(width, 600);
        panel_->setGeometry(0, 0, width, panel_->heightForWidth(width));
        panel_->layout()->activate();
        for (int i = 0; i < 4; ++i) {
            panel_->row(i)->layout()->activate();
        }
    }
    [[nodiscard]] InputPanel& panel() const { return *panel_; }

private:
    QWidget host_;
    InputPanel* panel_ = nullptr;
};

/// `widget`'s rectangle in `ancestor` coordinates.
[[nodiscard]] QRect rect_in(const QWidget* widget, const QWidget* ancestor)
{
    return {widget->mapTo(ancestor, QPoint(0, 0)), widget->size()};
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

// --- Layout and texts (01 §4.4 controls in the cards of D-53) ------------------------------

TEST(InputPanel, Cards_hold_the_controls_of_01_4_4_in_reading_order)
{
    // 01 §4.3's three wrapping rows became four titled cards in a 2 x 2 grid (D-53): Where,
    // Travellers & prices, Outbound, Return; "Outbound between" / "Return between" became
    // the card caption plus "between" (D-54).
    const auto panel = make_panel();
    using V = std::vector<std::string>;
    const std::array<const char*, 4> names{"whereCard", "travellersCard", "outboundCard", "returnCard"};
    for (int i = 0; i < 4; ++i) {
        ASSERT_NE(card(*panel, i), nullptr) << i;
        EXPECT_EQ(card(*panel, i)->objectName(), QString::fromLatin1(names[static_cast<std::size_t>(i)]));
    }
    EXPECT_EQ(card(*panel, 0)->title_label()->text(), QStringLiteral("Where"));
    EXPECT_EQ(card(*panel, 1)->title_label()->text(), QStringLiteral("Travellers & prices"));
    EXPECT_EQ(card(*panel, 2)->title_label()->text(), QStringLiteral("Outbound"));
    EXPECT_EQ(card(*panel, 3)->title_label()->text(), QStringLiteral("Return"));
    EXPECT_EQ(panel->row(4), nullptr);

    EXPECT_EQ(card_items(card(*panel, 0)),
              (V{"(icon)", "Where", "(empty “To” = anywhere)", "From", "[wizz::app::PlaceCombo]", "within",
                 "[wizz::app::FractionalSpinBox]", "km", "To", "[wizz::app::PlaceCombo]", "within",
                 "[wizz::app::FractionalSpinBox]", "km"}));
    EXPECT_EQ(card_items(card(*panel, 1)), (V{"(icon)", "Travellers & prices", "Adults",
                                              "[wizz::app::FractionalSpinBox]", "[QComboBox]", "Prices in", "[QComboBox]"}));
    EXPECT_EQ(card_items(card(*panel, 2)), (V{"(icon)", "Outbound", "between", "[wizz::app::DatePicker]", "and",
                                              "[wizz::app::DatePicker]", "[wizz::app::SearchButton]"}));
    EXPECT_EQ(card_items(card(*panel, 3)),
              (V{"(icon)", "Return", "Return flights go from the destination area back to any airport near home.",
                 "between", "[wizz::app::DatePicker]", "and", "[wizz::app::DatePicker]", "Round trips: stay",
                 "[wizz::app::FractionalSpinBox]", "to", "[wizz::app::FractionalSpinBox]", "nights",
                 "[wizz::app::SearchButton]"}));

    // The controls live in the card they are listed in.
    const auto in_card = [&panel](int index, const QWidget* widget) { return card(*panel, index)->isAncestorOf(widget); };
    EXPECT_TRUE(in_card(0, panel->from_box()));
    EXPECT_TRUE(in_card(0, panel->to_radius_box()));
    EXPECT_TRUE(in_card(1, panel->adults_box()));
    EXPECT_TRUE(in_card(1, panel->fare_box()));
    EXPECT_TRUE(in_card(1, panel->currency_box()));
    EXPECT_TRUE(in_card(2, panel->out_to_picker()));
    EXPECT_TRUE(in_card(2, panel->search_outbound_button()));
    EXPECT_TRUE(in_card(3, panel->ret_from_picker()));
    EXPECT_TRUE(in_card(3, panel->max_nights_box()));
    EXPECT_TRUE(in_card(3, panel->search_return_button()));
    EXPECT_FALSE(card(*panel, 0)->isAncestorOf(panel->resolved_label())) << "the resolved line is under the cards";
}

TEST(InputPanel, Cards_sit_in_a_2_by_2_grid_with_the_card_paddings)
{
    const HostedPanel hosted{1288};
    InputPanel& panel = hosted.panel();
    EXPECT_EQ(panel.layout()->contentsMargins(), QMargins(16, 6, 16, 0));
    const QRect where = panel.row(0)->geometry();
    const QRect travellers = panel.row(1)->geometry();
    const QRect outbound = panel.row(2)->geometry();
    const QRect inbound = panel.row(3)->geometry();
    EXPECT_EQ(where.topLeft(), QPoint(16, 6));
    EXPECT_EQ(travellers.top(), where.top());
    EXPECT_EQ(outbound.left(), where.left());
    EXPECT_EQ(inbound.left(), travellers.left());
    EXPECT_EQ(travellers.left() - where.right() - 1, 8) << "8 px between the columns";
    EXPECT_EQ(outbound.top() - where.bottom() - 1, 8) << "8 px between the rows";
    EXPECT_EQ(inbound.top(), outbound.top());
    EXPECT_EQ(travellers.right(), 1288 - 16 - 1);
    EXPECT_EQ(where.height(), travellers.height()) << "a grid row is as tall as its taller card";
    EXPECT_EQ(outbound.height(), inbound.height());
    EXPECT_GT(where.width(), travellers.width()) << "the place boxes' column takes the extra width (3:2)";

    for (int i = 0; i < 4; ++i) {
        const auto* section = card(panel, i);
        EXPECT_EQ(section->layout()->contentsMargins(), wizz::app::SectionCard::kPadding) << i;
        // Caption: the 8 pt upper-case semibold font in the secondary text colour; hints muted, 9 pt.
        EXPECT_EQ(section->title_label()->font(), wizz::app::style::caption_font()) << i;
        EXPECT_EQ(section->title_label()->palette().color(QPalette::WindowText),
                  wizz::app::style::theme().text_secondary);
        if (const QLabel* hint = section->hint_label()) {
            EXPECT_EQ(hint->font(), wizz::app::style::hint_font());
            EXPECT_EQ(hint->palette().color(QPalette::WindowText), wizz::app::style::theme().text_muted);
        }
    }
    EXPECT_EQ(wizz::app::SectionCard::kPadding, QMargins(14, 6, 14, 8));
    EXPECT_EQ(card(panel, 0)->hint_label()->text(), QStringLiteral("(empty “To” = anywhere)"));
    EXPECT_EQ(card(panel, 3)->hint_label()->text().toStdString(),
              "Return flights go from the destination area back to any airport near home.");
    EXPECT_EQ(card(panel, 1)->hint_label(), nullptr);

    // Field labels in the secondary text colour, never focusable.
    for (const QLabel* label : panel.findChildren<QLabel*>()) {
        EXPECT_EQ(label->focusPolicy(), Qt::NoFocus) << label->text().toStdString();
    }
    const auto* from_label = panel.row(0)->findChildren<QLabel*>().value(2);  // caption, hint, then "From"
    ASSERT_NE(from_label, nullptr);
    EXPECT_EQ(from_label->text(), QStringLiteral("From"));
    EXPECT_EQ(from_label->palette().color(QPalette::WindowText), wizz::app::style::theme().text_secondary);

    // The resolved line: 4 px under the cards, muted 9 pt, wrapping, plain text.
    QLabel* resolved = panel.resolved_label();
    EXPECT_EQ(resolved->text(),
              QStringLiteral("Fares come from Wizz Air's website. Double-click a result to open it on wizzair.com."));
    EXPECT_TRUE(resolved->wordWrap());
    EXPECT_EQ(resolved->textFormat(), Qt::PlainText);
    EXPECT_EQ(resolved->font(), wizz::app::style::hint_font());
    EXPECT_EQ(resolved->contentsMargins(), QMargins(0, 0, 4, 0));
    EXPECT_EQ(resolved->palette().color(QPalette::WindowText), wizz::app::style::gray_text());
    EXPECT_EQ(rect_in(resolved, &panel).top(), outbound.bottom() + 1 + 4);
    panel.set_resolved_text("Outbound: from Budapest (BUD), Hungary → airports within 250 km: BUD 0 km.   To: anywhere.");
    EXPECT_EQ(panel.resolved_text().toStdString(),
              "Outbound: from Budapest (BUD), Hungary → airports within 250 km: BUD 0 km.   To: anywhere.");
}

TEST(InputPanel, Widths_heights_ranges_steps_and_tooltips_follow_the_table)
{
    const HostedPanel hosted{1288};
    InputPanel& panel = hosted.panel();
    // The fixed widths of the look (D-53; 01 §4.4: 64, 130, 48, 175, 70, 52).
    EXPECT_EQ(panel.from_radius_box()->width(), 76);
    EXPECT_EQ(panel.to_radius_box()->width(), 76);
    for (DatePicker* picker :
         {panel.out_from_picker(), panel.out_to_picker(), panel.ret_from_picker(), panel.ret_to_picker()}) {
        EXPECT_EQ(picker->width(), 142);
        EXPECT_EQ(picker->bounds().min, kToday);
        EXPECT_EQ(picker->bounds().max, Date::from_ymd(2028, 11, 2));
        EXPECT_EQ(picker->displayFormat(), QStringLiteral("ddd dd MMM yyyy"));
    }
    EXPECT_EQ(panel.adults_box()->width(), 64);
    EXPECT_EQ(panel.fare_box()->width(), 206);
    EXPECT_EQ(panel.currency_box()->width(), 88);
    EXPECT_EQ(panel.min_nights_box()->width(), 64);
    EXPECT_EQ(panel.max_nights_box()->width(), 64);
    // The place boxes stretch with the window (01: 210 fixed), never below 180.
    EXPECT_EQ(panel.from_box()->minimumWidth(), 180);
    EXPECT_EQ(panel.from_box()->sizePolicy().horizontalPolicy(), QSizePolicy::Expanding);
    EXPECT_EQ(panel.to_box()->width(), panel.from_box()->width());
    EXPECT_GT(panel.from_box()->width(), 180);

    // Every input and both search buttons are ModernStyle::control_height tall, so a row of
    // fields and its button line up.
    const int control = wizz::app::ModernStyle::control_height(QFontMetrics(wizz::app::style::form_font()));
    for (const QWidget* input : panel.tab_order()) {
        EXPECT_EQ(input->height(), control) << input->metaObject()->className();
    }
    EXPECT_EQ(rect_in(panel.out_from_picker(), &panel).center().y(), rect_in(panel.out_to_picker(), &panel).center().y());

    const auto expect_numeric = [](const FractionalSpinBox* box, double minimum, double maximum, double step) {
        EXPECT_EQ(box->minimum(), minimum);
        EXPECT_EQ(box->maximum(), maximum);
        EXPECT_EQ(box->single_step(), step);
    };
    expect_numeric(panel.from_radius_box(), 0, 1500, 25);
    expect_numeric(panel.to_radius_box(), 0, 1500, 25);
    expect_numeric(panel.adults_box(), 1, 9, 1);
    expect_numeric(panel.min_nights_box(), 0, 60, 1);
    expect_numeric(panel.max_nights_box(), 0, 120, 1);

    EXPECT_EQ(panel.from_box()->toolTip().toStdString(),
              "City or airport you start from (any city works — nearby Wizz Air airports are found automatically)");
    EXPECT_EQ(panel.to_box()->toolTip().toStdString(),
              "Destination city or airport. Leave empty to search ANYWHERE Wizz Air flies.");
    EXPECT_EQ(panel.search_outbound_button()->text(), QStringLiteral("Search outbound"));
    EXPECT_EQ(panel.search_return_button()->text(), QStringLiteral("Search return"));
    EXPECT_FALSE(panel.fare_box()->isEditable());
    EXPECT_FALSE(panel.currency_box()->isEditable());
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

TEST(InputPanel, Tab_order_follows_the_cards_D_54)
{
    // D-54: Tab walks the cards as they are read — Where, Travellers & prices, Outbound,
    // Return — each left to right, top to bottom. (01 §6.3 had the outbound dates before
    // adults, fare and currency.)
    const HostedPanel hosted{1288};
    InputPanel& panel = hosted.panel();
    const std::vector<QWidget*> expected{panel.from_box(),        panel.from_radius_box(),
                                         panel.to_box(),          panel.to_radius_box(),
                                         panel.adults_box(),      panel.fare_box(),
                                         panel.currency_box(),    panel.out_from_picker(),
                                         panel.out_to_picker(),   panel.search_outbound_button(),
                                         panel.ret_from_picker(), panel.ret_to_picker(),
                                         panel.min_nights_box(),  panel.max_nights_box(),
                                         panel.search_return_button()};
    EXPECT_EQ(panel.tab_order(), expected);

    // Walk the focus chain from "From" and keep the listed widgets in visiting order.
    std::vector<QWidget*> visited;
    QWidget* widget = panel.from_box();
    for (int guard = 0; guard < 500 && visited.size() < expected.size(); ++guard) {
        if (std::ranges::find(expected, widget) != expected.end() &&
            std::ranges::find(visited, widget) == visited.end()) {
            visited.push_back(widget);
        }
        widget = widget->nextInFocusChain();
    }
    EXPECT_EQ(visited, expected);

    // The order is the reading order of the laid-out cards: card by card, and inside a
    // card row by row, left to right.
    const auto reading_key = [&panel](const QWidget* control) {
        int index = 0;
        while (index < 4 && !panel.row(index)->isAncestorOf(control)) {
            ++index;
        }
        const QRect rect = rect_in(control, panel.row(std::min(index, 3)));
        return std::tuple{index, rect.center().y(), rect.left()};
    };
    for (std::size_t i = 1; i < expected.size(); ++i) {
        const auto before = reading_key(expected[i - 1]);
        const auto after = reading_key(expected[i]);
        EXPECT_LT(before, after) << expected[i - 1]->metaObject()->className() << " before "
                                 << expected[i]->metaObject()->className() << " (" << i << ")";
        EXPECT_LT(std::get<0>(after), 4) << "in a card";
    }
}

// --- The cards at narrow widths (the 980 x 600 minimum window) ------------------------------

TEST(InputPanel, The_cards_fit_the_minimum_window)
{
    // D-53: at the client width of the 980 x 600 window every card keeps its controls, at
    // their size, inside its padding (nothing clipped, no label cut), the 2 x 2 grid
    // stays, and both search buttons sit at the right end of their cards' last row.
    const int minimum = minimum_client_size().width();
    ASSERT_EQ(minimum, 964) << "980 minus the 8 px borders";
    HostedPanel hosted{1288};
    InputPanel& panel = hosted.panel();
    EXPECT_LE(panel.minimumSizeHint().width(), minimum) << "the cards' minimum fits the minimum window";
    const auto expect_inside_its_card = [&panel](const char* where) {
        for (int i = 0; i < 4; ++i) {
            const QWidget* section = panel.row(i);
            const QRect inner = section->rect().marginsRemoved(wizz::app::SectionCard::kPadding);
            for (const QWidget* child : section->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
                if (!child->isVisible()) {
                    continue;
                }
                const auto* label = qobject_cast<const QLabel*>(child);
                EXPECT_TRUE(inner.contains(child->geometry()))
                    << where << ": " << child->metaObject()->className() << " "
                    << (label != nullptr ? label->text().toStdString() : std::string{}) << " outside card " << i;
                if (label != nullptr && !label->wordWrap()) {
                    EXPECT_GE(label->width(), label->sizeHint().width())
                        << where << ": cut text " << label->text().toStdString();
                }
            }
        }
    };
    const auto expect_search_buttons_at_the_right_end = [&panel](const char* where) {
        for (const auto& [index, button] : {std::pair{2, static_cast<QWidget*>(panel.search_outbound_button())},
                                            std::pair{3, static_cast<QWidget*>(panel.search_return_button())}}) {
            const QWidget* section = panel.row(index);
            const QRect rect = rect_in(button, section);
            EXPECT_EQ(rect.right(), section->width() - wizz::app::SectionCard::kPadding.right() - 1) << where;
            EXPECT_NEAR(rect.bottom(), section->height() - wizz::app::SectionCard::kPadding.bottom() - 1, 1)
                << where << ": on the card's last row";
            EXPECT_EQ(button->size(), button->sizeHint()) << where << ": never squeezed";
        }
    };

    expect_inside_its_card("default window");
    expect_search_buttons_at_the_right_end("default window");

    hosted.place(minimum);
    expect_inside_its_card("minimum window");
    expect_search_buttons_at_the_right_end("minimum window");
    EXPECT_GE(panel.from_box()->width(), 180);
    EXPECT_EQ(panel.row(1)->geometry().top(), panel.row(0)->geometry().top()) << "still 2 x 2";
    EXPECT_GT(panel.row(2)->geometry().top(), panel.row(0)->geometry().bottom());
    save_render(panel, QStringLiteral("input-panel-minimum"));
}

} // namespace
