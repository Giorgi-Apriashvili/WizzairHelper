#pragma once
// The date picker of the input panel (01 §4.4): a QDateEdit with a calendar
// drop-down, custom format "ddd dd MMM yyyy" (e.g. "Fri 20 Nov 2026"), and the
// range [MinDate, MaxDate] = [start-up day, start-up day + 2 calendar years].
//
// The text is rendered by wizz::Locale (07 §1: month and weekday names come
// from the single locale service, so the GUI follows the Windows user locale
// exactly like the grids and logs); the widget's QLocale is set to the same
// locale name, which drives typing, section stepping and the calendar. Where
// Qt's own abbreviations differ from the locale service's (de-DE weekday "Mi."
// vs "Mi"), Qt's parser still finds the sections but only half-matches the name;
// validate() and dateTimeFromText() then accept the text through the locale
// service's names (date_from_text), so typing works in every locale.
//
// Every programmatic assignment is clamped to the range (01 §6.1 Clamp(p, v),
// UI-11, UX-16). dateChanged fires for user and code changes alike, only when
// the date really changes.
//
// The calendar drop-down (DECISIONS D-54) has no week-number column; weekends are
// in the theme's accent text colour (Qt's default pure red is 4.0:1 on white,
// below AA), days of the neighbouring months in its muted text (Qt's disabled
// colour fails AA too, and those days can be picked). High Contrast keeps the
// system colours. The formats follow changes of the look.

#include "wizz/core/locale.hpp"
#include "wizz/core/time.hpp"
#include "wizz/ui/date_rules.hpp"

#include <QDate>
#include <QDateEdit>
#include <QLocale>

#include <memory>
#include <optional>

namespace wizz::app {

[[nodiscard]] QDate to_qdate(Date date);
[[nodiscard]] Date from_qdate(QDate date);
/// The QLocale for a wizz::Locale ("de-DE" → QLocale("de_DE")).
[[nodiscard]] QLocale to_qlocale(const Locale& locale);

class DatePicker final : public QDateEdit {
    Q_OBJECT

public:
    /// `locale` renders the names (must not be null).
    explicit DatePicker(std::shared_ptr<const Locale> locale, QWidget* parent = nullptr);

    /// MinDate / MaxDate; the current value is clamped into the range.
    void set_bounds(const ui::DateBounds& bounds);
    [[nodiscard]] ui::DateBounds bounds() const;

    [[nodiscard]] Date value() const;
    /// `p.Value = Clamp(p, value)`.
    void set_value(Date value);

    /// The display text of `date` ("Fri 20 Nov 2026" in en-US).
    [[nodiscard]] QString text_for(Date date) const;
    /// Parses "<weekday> <d or dd> <month> <yyyy>" written with the locale service's
    /// names; the weekday must be a valid name but the date decides it (as Qt does
    /// while a day is being typed). nullopt for anything else or a date outside the range.
    [[nodiscard]] std::optional<Date> date_from_text(const QString& text) const;

    // QDateEdit
    [[nodiscard]] QValidator::State validate(QString& input, int& pos) const override;

protected:
    [[nodiscard]] QString textFromDateTime(const QDateTime& date_time) const override;
    [[nodiscard]] QDateTime dateTimeFromText(const QString& text) const override;

private:
    /// Weekend and neighbouring-month colours of the calendar drop-down.
    void apply_calendar_look();

    std::shared_ptr<const Locale> locale_;
};

} // namespace wizz::app
