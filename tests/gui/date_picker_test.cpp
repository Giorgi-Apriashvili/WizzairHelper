// DatePicker: "ddd dd MMM yyyy" through the locale service, range [today,
// today + 2 years], clamped programmatic assignments (01 §4.4, §6.1, 07 §1).

#include "date_picker.hpp"

#include "input_test_support.hpp"

#include "wizz/core/locale.hpp"
#include "wizz/ui/date_rules.hpp"

#include <QCalendarWidget>
#include <QLineEdit>

#include <gtest/gtest.h>

#include <vector>

namespace {

using wizz::Date;
using wizz::Locale;
using wizz::app::DatePicker;
using wizz::app::from_qdate;
using wizz::app::to_qdate;
using namespace wizz::test::gui;

/// What a click into a section does with real focus: select its text.
void select_text(DatePicker& picker, int start, int length)
{
    picker.findChild<QLineEdit*>()->setSelection(start, length);
}

[[nodiscard]] wizz::ui::DateBounds start_up_bounds()
{
    return wizz::ui::DateBounds{kToday, wizz::ui::add_years(kToday, 2)};
}

TEST(DatePicker, Converts_dates_and_locales)
{
    const Date date = Date::from_ymd(2028, 2, 29);
    EXPECT_EQ(to_qdate(date), QDate(2028, 2, 29));
    EXPECT_EQ(from_qdate(QDate(2028, 2, 29)), date);
    EXPECT_EQ(wizz::app::to_qlocale(*Locale::en_us()).name(), QStringLiteral("en_US"));
    EXPECT_EQ(wizz::app::to_qlocale(*Locale::create("de-DE")).name(), QStringLiteral("de_DE"));
}

TEST(DatePicker, Shows_ddd_dd_MMM_yyyy_with_a_calendar_drop_down)
{
    DatePicker picker(Locale::en_us());
    picker.set_bounds(start_up_bounds());
    picker.set_value(Date::from_ymd(2026, 11, 20));
    EXPECT_EQ(picker.text(), QStringLiteral("Fri 20 Nov 2026"));  // 01 §4.4 example
    picker.set_value(Date::from_ymd(2026, 12, 5));
    EXPECT_EQ(picker.text(), QStringLiteral("Sat 05 Dec 2026"));
    EXPECT_EQ(picker.displayFormat(), QStringLiteral("ddd dd MMM yyyy"));
    EXPECT_TRUE(picker.calendarPopup());
    ASSERT_NE(picker.calendarWidget(), nullptr);
    EXPECT_EQ(picker.calendarWidget()->locale().name(), QStringLiteral("en_US"));
    EXPECT_EQ(picker.locale().name(), QStringLiteral("en_US"));
}

TEST(DatePicker, Range_is_today_to_today_plus_two_calendar_years_and_every_set_is_clamped)
{
    DatePicker picker(Locale::en_us());
    picker.set_bounds(start_up_bounds());
    EXPECT_EQ(picker.bounds().min, kToday);
    EXPECT_EQ(picker.bounds().max, Date::from_ymd(2028, 11, 2));

    picker.set_value(Date::from_ymd(2026, 10, 1));
    EXPECT_EQ(picker.value(), kToday);
    picker.set_value(Date::from_ymd(2031, 1, 1));
    EXPECT_EQ(picker.value(), Date::from_ymd(2028, 11, 2));
    picker.setDate(QDate(2020, 1, 1));  // even Qt's own setter cannot leave the range
    EXPECT_EQ(picker.value(), kToday);

    // Leap day: AddYears(2) of 29 Feb 2028 is 28 Feb 2030.
    picker.set_bounds({Date::from_ymd(2028, 2, 29), wizz::ui::add_years(Date::from_ymd(2028, 2, 29), 2)});
    EXPECT_EQ(picker.bounds().max, Date::from_ymd(2030, 2, 28));
}

TEST(DatePicker, DateChanged_fires_only_for_real_changes)
{
    DatePicker picker(Locale::en_us());
    picker.set_bounds(start_up_bounds());
    picker.set_value(Date::from_ymd(2026, 11, 9));
    std::vector<QDate> changes;
    QObject::connect(&picker, &QDateEdit::dateChanged, [&changes](QDate date) { changes.push_back(date); });
    picker.set_value(Date::from_ymd(2026, 11, 9));
    EXPECT_TRUE(changes.empty());
    picker.set_value(Date::from_ymd(2026, 11, 12));
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes.front(), QDate(2026, 11, 12));
    picker.set_value(Date::from_ymd(2026, 1, 1));  // clamped to today
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes.back(), QDate(2026, 11, 2));
}

TEST(DatePicker, Sections_step_day_month_and_year)
{
    DatePicker picker(Locale::en_us());
    picker.set_bounds(start_up_bounds());
    picker.set_value(Date::from_ymd(2026, 11, 20));
    picker.setCurrentSectionIndex(1);  // dd
    picker.stepBy(1);
    EXPECT_EQ(picker.value(), Date::from_ymd(2026, 11, 21));
    EXPECT_EQ(picker.text(), QStringLiteral("Sat 21 Nov 2026"));
    picker.setCurrentSectionIndex(2);  // MMM
    picker.stepBy(1);
    EXPECT_EQ(picker.value(), Date::from_ymd(2026, 12, 21));
    picker.setCurrentSectionIndex(3);  // yyyy
    picker.stepBy(1);
    EXPECT_EQ(picker.value(), Date::from_ymd(2027, 12, 21));
    EXPECT_EQ(picker.text(), QStringLiteral("Tue 21 Dec 2027"));
}

TEST(DatePicker, Parses_text_written_with_the_locale_service_names)
{
    DatePicker picker(Locale::en_us());
    picker.set_bounds(start_up_bounds());
    EXPECT_EQ(picker.date_from_text(QStringLiteral("Fri 20 Nov 2026")), Date::from_ymd(2026, 11, 20));
    EXPECT_EQ(picker.date_from_text(QStringLiteral("Mon 5 Dec 2026")), Date::from_ymd(2026, 12, 5));  // the date decides the weekday
    EXPECT_EQ(picker.date_from_text(QStringLiteral("Tue 02 Nov 2028")), Date::from_ymd(2028, 11, 2));
    EXPECT_FALSE(picker.date_from_text(QStringLiteral("Xyz 20 Nov 2026")));
    EXPECT_FALSE(picker.date_from_text(QStringLiteral("Fri 31 Nov 2026")));  // no such day
    EXPECT_FALSE(picker.date_from_text(QStringLiteral("Fri 20 Nov 2025")));  // before MinDate
    EXPECT_FALSE(picker.date_from_text(QStringLiteral("Fri 20 Nov 2029")));  // after MaxDate
    EXPECT_FALSE(picker.date_from_text(QStringLiteral("Fri 20 November 2026")));
    EXPECT_FALSE(picker.date_from_text(QStringLiteral("Fri 200 Nov 2026")));
    EXPECT_FALSE(picker.date_from_text(QStringLiteral("Fri 20 Nov 26")));

    DatePicker german(Locale::create("de-DE"));
    german.set_bounds(start_up_bounds());
    EXPECT_EQ(german.date_from_text(QString::fromUtf8("Mi 05 Dez. 2026")), Date::from_ymd(2026, 12, 5));
}

TEST(DatePicker, Typing_a_section_changes_the_date)
{
    DatePicker picker(Locale::en_us());
    picker.set_bounds(start_up_bounds());
    picker.set_value(Date::from_ymd(2026, 11, 20));
    select_text(picker, 4, 2);  // "Fri [20] Nov 2026"
    type_text(&picker, QStringLiteral("25"));
    send_key(&picker, Qt::Key_Return);
    EXPECT_EQ(picker.value(), Date::from_ymd(2026, 11, 25));
    EXPECT_EQ(picker.text(), QStringLiteral("Wed 25 Nov 2026"));

    select_text(picker, 11, 4);  // "Wed 25 Nov [2026]"
    type_text(&picker, QStringLiteral("2027"));
    send_key(&picker, Qt::Key_Return);
    EXPECT_EQ(picker.value(), Date::from_ymd(2027, 11, 25));
    EXPECT_EQ(picker.text(), QStringLiteral("Thu 25 Nov 2027"));
}

TEST(DatePicker, Names_come_from_the_locale_service)
{
    const auto german = Locale::create("de-DE");
    DatePicker picker(german);
    picker.set_bounds(start_up_bounds());
    const Date date = Date::from_ymd(2026, 11, 20);
    picker.set_value(date);
    EXPECT_EQ(picker.text().toStdString(), german->format_date(date, "ddd dd MMM yyyy"));
    EXPECT_EQ(picker.text_for(date).toStdString(), german->format_date(date, "ddd dd MMM yyyy"));
    EXPECT_EQ(picker.locale().name(), QStringLiteral("de_DE"));

    // Stepping keeps working when the locale service names differ from Qt's own.
    picker.setCurrentSectionIndex(1);
    picker.stepBy(3);
    EXPECT_EQ(picker.value(), Date::from_ymd(2026, 11, 23));
    EXPECT_EQ(picker.text().toStdString(), german->format_date(Date::from_ymd(2026, 11, 23), "ddd dd MMM yyyy"));
    picker.setCurrentSectionIndex(2);
    picker.stepBy(1);
    EXPECT_EQ(picker.value(), Date::from_ymd(2026, 12, 23));

    // … and so does typing a section.
    select_text(picker, 3, 2);  // "Mi [23] Dez. 2026"
    type_text(&picker, QStringLiteral("05"));
    send_key(&picker, Qt::Key_Return);
    EXPECT_EQ(picker.value(), Date::from_ymd(2026, 12, 5));
    EXPECT_EQ(picker.text().toStdString(), german->format_date(Date::from_ymd(2026, 12, 5), "ddd dd MMM yyyy"));
}

} // namespace
