#pragma once
// The numeric editor of the input panel: a WinForms NumericUpDown with
// DecimalPlaces 0 that is not integer-only (01 §4.4 "Numeric up-downs are not
// integer-only"):
//   * it displays the value rounded half away from zero, without decimals or
//     digit grouping (2.5 shows "3");
//   * typed text is parsed like Decimal.Parse(text, CurrentCulture) with
//     NumberStyles.Number (the locale's decimal and group separators, a leading
//     or trailing sign, surrounding white space) and committed on Enter, focus
//     loss or a step when the user edited it (NumericUpDown.UserEdit); the
//     fraction is kept, the value is clamped to the range, and text that is not
//     a number reverts to the current value;
//   * value() keeps the fraction (radii search with it); int_value() truncates
//     toward zero (adults, night filters, persisted integers);
//   * valueChanged fires only when the committed value really changes.
// Keyboard tracking is off: like NumericUpDown, typing alone changes nothing.

#include "wizz/core/locale.hpp"

#include <QAbstractSpinBox>
#include <QString>

#include <optional>

namespace wizz::app {

class FractionalSpinBox final : public QAbstractSpinBox {
    Q_OBJECT

public:
    explicit FractionalSpinBox(QWidget* parent = nullptr);

    /// Range (minimum ≤ maximum); the current value is clamped into it.
    void set_range(double minimum, double maximum);
    [[nodiscard]] double minimum() const noexcept { return minimum_; }
    [[nodiscard]] double maximum() const noexcept { return maximum_; }
    /// The up/down increment (NumericUpDown.Increment).
    void set_single_step(double step) noexcept { step_ = step; }
    [[nodiscard]] double single_step() const noexcept { return step_; }
    /// Separators of typed text (wizz::Locale::number_symbols(); en-US by default).
    void set_number_symbols(NumberSymbols symbols);
    [[nodiscard]] const NumberSymbols& number_symbols() const noexcept { return symbols_; }

    /// The committed value, fraction included.
    [[nodiscard]] double value() const noexcept { return value_; }
    /// The value truncated toward zero.
    [[nodiscard]] int int_value() const noexcept;
    /// Sets the value clamped to the range and shows it rounded; emits valueChanged
    /// when it changes. NaN is ignored.
    void set_value(double value);
    /// Commits text the user typed since the last commit (what Enter, focus loss and
    /// a step do); nothing happens when the text was not edited.
    void commit();
    /// Whether the user edited the text since the last commit (NumericUpDown.UserEdit).
    [[nodiscard]] bool user_edit() const noexcept { return user_edit_; }

    /// The display text of `value`: rounded half away from zero, ASCII digits.
    [[nodiscard]] QString text_from_value(double value) const;
    /// Decimal.Parse(text, culture) with NumberStyles.Number; nullopt when `text` is
    /// not a number. The result is not clamped.
    [[nodiscard]] std::optional<double> value_from_text(const QString& text) const;

    // QAbstractSpinBox
    void stepBy(int steps) override;
    [[nodiscard]] QValidator::State validate(QString& input, int& pos) const override;
    void fixup(QString& input) const override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

Q_SIGNALS:
    void valueChanged(double value);

protected:
    [[nodiscard]] StepEnabled stepEnabled() const override;

private:
    [[nodiscard]] bool is_allowed_character(QChar c) const;
    void update_text();

    double minimum_ = 0;
    double maximum_ = 99;
    double step_ = 1;
    double value_ = 0;
    bool user_edit_ = false;
    NumberSymbols symbols_;
};

} // namespace wizz::app
