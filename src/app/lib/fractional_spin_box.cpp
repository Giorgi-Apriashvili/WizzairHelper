#include "fractional_spin_box.hpp"

#include "qt_strings.hpp"

#include "wizz/ui/settings.hpp"

#include <QFontMetrics>
#include <QLineEdit>
#include <QStringView>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <system_error>
#include <utility>

namespace wizz::app {

namespace {

/// NumberStyles.AllowLeadingWhite / AllowTrailingWhite: U+0009–U+000D and U+0020.
[[nodiscard]] bool is_number_white(QChar c) noexcept
{
    return c == u' ' || (c >= u'\t' && c <= u'\r');
}

[[nodiscard]] bool is_ascii_digit(QChar c) noexcept
{
    return c >= u'0' && c <= u'9';
}

/// .NET's MatchChars: the separator at `position` of `text`, where a typed space
/// stands for a no-break space (U+00A0) or narrow no-break space (U+202F) in the
/// culture's separator. Returns the matched length (0 = no match).
[[nodiscard]] qsizetype match_symbol(QStringView text, qsizetype position, QStringView symbol) noexcept
{
    if (symbol.isEmpty() || position + symbol.size() > text.size()) {
        return 0;
    }
    for (qsizetype k = 0; k < symbol.size(); ++k) {
        const QChar typed = text[position + k];
        const QChar wanted = symbol[k];
        const bool space_for_nbsp = typed == u' ' && (wanted == QChar(0x00A0) || wanted == QChar(0x202F));
        if (typed != wanted && !space_for_nbsp) {
            return 0;
        }
    }
    return symbol.size();
}

} // namespace

FractionalSpinBox::FractionalSpinBox(QWidget* parent) : QAbstractSpinBox(parent)
{
    setKeyboardTracking(false);
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    setCorrectionMode(QAbstractSpinBox::CorrectToPreviousValue);
    connect(lineEdit(), &QLineEdit::textEdited, this, [this] { user_edit_ = true; });
    connect(this, &QAbstractSpinBox::editingFinished, this, &FractionalSpinBox::commit);
    update_text();
}

void FractionalSpinBox::set_range(double minimum, double maximum)
{
    minimum_ = minimum;
    maximum_ = std::max(minimum, maximum);
    const double clamped = std::clamp(value_, minimum_, maximum_);
    if (clamped != value_) {
        set_value(clamped);
    }
    updateGeometry();
}

void FractionalSpinBox::set_number_symbols(NumberSymbols symbols)
{
    symbols_ = std::move(symbols);
}

int FractionalSpinBox::int_value() const noexcept
{
    return ui::truncate_toward_zero(value_);
}

void FractionalSpinBox::set_value(double value)
{
    if (std::isnan(value)) {
        update_text();
        return;
    }
    const double clamped = std::clamp(value, minimum_, maximum_);
    const bool changed = clamped != value_;
    value_ = clamped;
    update_text();
    if (changed) {
        Q_EMIT valueChanged(value_);
    }
}

void FractionalSpinBox::commit()
{
    if (!user_edit_) {
        return;  // the text shows the (rounded) value; parsing it would drop the fraction
    }
    user_edit_ = false;
    if (const std::optional<double> parsed = value_from_text(lineEdit()->text())) {
        set_value(*parsed);
    } else {
        update_text();  // not a number: show the current value again
    }
}

QString FractionalSpinBox::text_from_value(double value) const
{
    return QString::number(static_cast<qlonglong>(ui::numeric_display_value(value)));
}

std::optional<double> FractionalSpinBox::value_from_text(const QString& text) const
{
    const QString decimal = to_qstring(symbols_.decimal_separator);
    const QString group = to_qstring(symbols_.group_separator);
    const QString negative = to_qstring(symbols_.negative_sign);
    const QString positive = QStringLiteral("+");

    QStringView view{text};
    while (!view.isEmpty() && is_number_white(view.front())) {
        view = view.sliced(1);
    }
    while (!view.isEmpty() && is_number_white(view.back())) {
        view.chop(1);
    }

    qsizetype position = 0;
    bool is_negative = false;
    bool has_sign = false;
    if (const qsizetype n = match_symbol(view, position, negative); n > 0) {
        is_negative = true;
        has_sign = true;
        position += n;
    } else if (const qsizetype p = match_symbol(view, position, positive); p > 0) {
        has_sign = true;
        position += p;
    }

    std::string number;
    bool has_digits = false;
    bool in_fraction = false;
    while (position < view.size()) {
        const QChar c = view[position];
        if (is_ascii_digit(c)) {
            number.push_back(static_cast<char>(c.unicode()));
            has_digits = true;
            ++position;
            continue;
        }
        if (!in_fraction) {
            if (const qsizetype n = match_symbol(view, position, decimal); n > 0) {
                number.push_back('.');
                in_fraction = true;
                position += n;
                continue;
            }
            if (const qsizetype n = match_symbol(view, position, group); n > 0 && has_digits) {
                position += n;
                continue;
            }
        }
        break;
    }
    if (!has_sign && position < view.size()) {  // NumberStyles.AllowTrailingSign
        if (const qsizetype n = match_symbol(view, position, negative); n > 0) {
            is_negative = true;
            position += n;
        } else if (const qsizetype p = match_symbol(view, position, positive); p > 0) {
            position += p;
        }
    }
    if (!has_digits || position != view.size()) {
        return std::nullopt;
    }
    if (number.front() == '.') {
        number.insert(number.begin(), '0');
    }
    if (number.back() == '.') {
        number.push_back('0');
    }
    double value = 0;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value);
    if (error != std::errc{} || end != number.data() + number.size()) {
        return std::nullopt;
    }
    return is_negative ? -value : value;
}

void FractionalSpinBox::stepBy(int steps)
{
    commit();  // NumericUpDown.UpButton: parse the edit text first
    set_value(value_ + steps * step_);
    selectAll();
}

QValidator::State FractionalSpinBox::validate(QString& input, int& /*pos*/) const
{
    if (!std::ranges::all_of(input, [this](QChar c) { return is_allowed_character(c); })) {
        return QValidator::Invalid;
    }
    return value_from_text(input) ? QValidator::Acceptable : QValidator::Intermediate;
}

void FractionalSpinBox::fixup(QString& /*input*/) const
{
    // Nothing to repair: commit() reverts text that is not a number.
}

QSize FractionalSpinBox::sizeHint() const
{
    QSize hint = QAbstractSpinBox::sizeHint();  // frame and buttons around an empty text
    const QFontMetrics metrics(fontMetrics());
    hint.rwidth() += std::max(metrics.horizontalAdvance(text_from_value(minimum_)),
                              metrics.horizontalAdvance(text_from_value(maximum_)));
    return hint;
}

QSize FractionalSpinBox::minimumSizeHint() const
{
    return sizeHint();
}

QAbstractSpinBox::StepEnabled FractionalSpinBox::stepEnabled() const
{
    if (isReadOnly()) {
        return StepNone;
    }
    StepEnabled enabled = StepNone;
    if (value_ < maximum_) {
        enabled |= StepUpEnabled;
    }
    if (value_ > minimum_) {
        enabled |= StepDownEnabled;
    }
    return enabled;
}

bool FractionalSpinBox::is_allowed_character(QChar c) const
{
    // NumericUpDown accepts digits, the decimal and group separators and the
    // negative sign; white space and '+' are what Decimal.Parse also accepts.
    if (is_ascii_digit(c) || is_number_white(c) || c == u'+') {
        return true;
    }
    const auto in = [c](const std::string& symbol) { return to_qstring(symbol).contains(c); };
    return in(symbols_.decimal_separator) || in(symbols_.group_separator) || in(symbols_.negative_sign);
}

void FractionalSpinBox::update_text()
{
    user_edit_ = false;
    const QString text = text_from_value(value_);
    if (lineEdit()->text() != text) {
        lineEdit()->setText(text);
    }
}

} // namespace wizz::app
