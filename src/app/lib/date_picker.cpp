#include "date_picker.hpp"

#include "qt_strings.hpp"
#include "ui_style.hpp"

#include "wizz/ui/strings.hpp"

#include <QApplication>
#include <QCalendarWidget>
#include <QStringView>
#include <QTextCharFormat>

#include <chrono>
#include <stdexcept>
#include <utility>

namespace wizz::app {

QDate to_qdate(Date date)
{
    return QDate(date.year(), static_cast<int>(date.month()), static_cast<int>(date.day()));
}

Date from_qdate(QDate date)
{
    return Date::from_ymd(date.year(), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
}

QLocale to_qlocale(const Locale& locale)
{
    QString name = to_qstring(locale.name());
    name.replace(u'-', u'_');
    return QLocale(name);
}

DatePicker::DatePicker(std::shared_ptr<const Locale> names, QWidget* parent)
    : QDateEdit(parent), locale_(std::move(names))
{
    if (!locale_) {
        throw std::invalid_argument("DatePicker needs a locale");
    }
    setLocale(to_qlocale(*locale_));
    setDisplayFormat(to_qstring(ui::strings::kDatePickerFormat));
    setCalendarPopup(true);
    if (QCalendarWidget* calendar = calendarWidget()) {
        calendar->setLocale(locale());
        calendar->setFirstDayOfWeek(locale().firstDayOfWeek());
        calendar->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);  // no week numbers
        apply_calendar_look();
        style::on_theme_change(this, [this] { apply_calendar_look(); });
    }
}

void DatePicker::apply_calendar_look()
{
    QCalendarWidget* calendar = calendarWidget();
    if (calendar == nullptr) {
        return;
    }
    const style::Theme& theme = style::theme();
    QTextCharFormat weekend;
    if (!theme.high_contrast) {
        weekend.setForeground(theme.accent_text);
    }
    calendar->setWeekdayTextFormat(Qt::Saturday, weekend);
    calendar->setWeekdayTextFormat(Qt::Sunday, weekend);
    // QCalendarWidget draws the days of the neighbouring months in the disabled text colour.
    QPalette palette = calendar->palette();
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     theme.high_contrast ? QApplication::palette().color(QPalette::Disabled, QPalette::Text)
                                         : theme.text_muted);
    calendar->setPalette(palette);
}

void DatePicker::set_bounds(const ui::DateBounds& bounds)
{
    setDateRange(to_qdate(bounds.min), to_qdate(bounds.max));
}

ui::DateBounds DatePicker::bounds() const
{
    return ui::DateBounds{from_qdate(minimumDate()), from_qdate(maximumDate())};
}

Date DatePicker::value() const
{
    return from_qdate(date());
}

void DatePicker::set_value(Date value)
{
    const QDate clamped = to_qdate(bounds().clamp(value));
    if (clamped != date()) {
        setDate(clamped);
    }
}

QString DatePicker::text_for(Date date) const
{
    return to_qstring(locale_->format_date(date, ui::strings::kDatePickerFormat));
}

std::optional<Date> DatePicker::date_from_text(const QString& text) const
{
    // Weekday: one of the seven names of the locale service, then a space.
    qsizetype position = -1;
    for (unsigned day = 0; day < 7 && position < 0; ++day) {
        const QString name = to_qstring(locale_->day_abbrev(std::chrono::weekday{day}));
        if (text.startsWith(name + u' ')) {
            position = name.size() + 1;
        }
    }
    if (position < 0) {
        return std::nullopt;
    }
    // Day: one or two ASCII digits, then a space.
    const qsizetype day_end = text.indexOf(u' ', position);
    const qsizetype last_space = text.lastIndexOf(u' ');
    if (day_end < 0 || day_end <= position || day_end - position > 2 || last_space <= day_end) {
        return std::nullopt;
    }
    bool day_ok = false;
    bool year_ok = false;
    const int day = text.sliced(position, day_end - position).toInt(&day_ok);
    const QStringView year_text = QStringView{text}.sliced(last_space + 1);
    const int year = year_text.toInt(&year_ok);
    if (!day_ok || !year_ok || year_text.size() != 4) {
        return std::nullopt;
    }
    // Month: the "MMM" of "dd MMM yyyy" (format form next to the day number).
    const QStringView month_text = QStringView{text}.sliced(day_end + 1, last_space - day_end - 1);
    for (unsigned month = 1; month <= 12; ++month) {
        const QString rendered =
            to_qstring(locale_->format_date(Date::from_ymd(year, month, 1), "dd MMM yyyy"));  // "01 Dez. 2026"
        if (rendered.sliced(3, rendered.size() - 3 - 5) != month_text) {
            continue;
        }
        const QDate date(year, static_cast<int>(month), day);
        if (!date.isValid()) {
            return std::nullopt;
        }
        const Date value = from_qdate(date);
        const ui::DateBounds range = bounds();
        if (value < range.min || value > range.max) {
            return std::nullopt;
        }
        return value;
    }
    return std::nullopt;
}

QValidator::State DatePicker::validate(QString& input, int& pos) const
{
    // Qt's parser locates the sections (and keeps the cursor logic right) even when
    // it only half-matches a locale-service name; the locale service decides then.
    const QValidator::State state = QDateEdit::validate(input, pos);
    if (state == QValidator::Acceptable) {
        return state;
    }
    // Only complete text ("05", not a first typed digit "0" or "5" that Qt is still
    // waiting to complete); the weekday may be stale while a day is being typed.
    const std::optional<Date> date = date_from_text(input);
    if (!date) {
        return state;
    }
    const QString rendered = text_for(*date);
    const qsizetype weekday_length = to_qstring(locale_->day_abbrev(date->weekday())).size();
    return input.endsWith(rendered.sliced(weekday_length)) ? QValidator::Acceptable : state;
}

QDateTime DatePicker::dateTimeFromText(const QString& text) const
{
    if (const std::optional<Date> date = date_from_text(text)) {
        QDateTime result = dateTime();  // keeps the edit's own time zone and time of day
        result.setDate(to_qdate(*date));
        return result;
    }
    return QDateEdit::dateTimeFromText(text);
}

QString DatePicker::textFromDateTime(const QDateTime& date_time) const
{
    const QDate date = date_time.date();
    if (!date.isValid()) {
        return QDateEdit::textFromDateTime(date_time);
    }
    return text_for(from_qdate(date));
}

} // namespace wizz::app
