// FractionalSpinBox: "Numeric up-downs are not integer-only" (01 §4.4 notes, T-03).

#include "fractional_spin_box.hpp"

#include "input_test_support.hpp"

#include "wizz/core/locale.hpp"

#include <QLineEdit>

#include <gtest/gtest.h>

#include <limits>
#include <vector>

namespace {

using wizz::NumberSymbols;
using wizz::app::FractionalSpinBox;
using namespace wizz::test::gui;

[[nodiscard]] QLineEdit* editor(FractionalSpinBox& box)
{
    return box.findChild<QLineEdit*>();
}

/// Replaces the editor text by typing, then presses Enter (commit).
bool type_and_enter(FractionalSpinBox& box, const QString& text)
{
    editor(box)->selectAll();
    send_key(&box, Qt::Key_Delete);
    type_text(&box, text);
    return send_key(&box, Qt::Key_Return);
}

[[nodiscard]] NumberSymbols german()
{
    return wizz::Locale::create("de-DE")->number_symbols();
}

TEST(FractionalSpinBox, Displays_whole_numbers_rounded_half_away_from_zero)
{
    FractionalSpinBox box;
    box.set_range(0, 1500);
    EXPECT_EQ(box.text_from_value(2.5), QStringLiteral("3"));
    EXPECT_EQ(box.text_from_value(2.4999), QStringLiteral("2"));
    EXPECT_EQ(box.text_from_value(0.5), QStringLiteral("1"));
    EXPECT_EQ(box.text_from_value(1.5), QStringLiteral("2"));
    EXPECT_EQ(box.text_from_value(1499.5), QStringLiteral("1500"));
    EXPECT_EQ(box.text_from_value(1500), QStringLiteral("1500"));  // no digit grouping

    box.set_value(250.5);
    EXPECT_EQ(box.text(), QStringLiteral("251"));
    EXPECT_DOUBLE_EQ(box.value(), 250.5);  // the fraction is kept
    EXPECT_EQ(box.int_value(), 250);       // truncated toward zero
    EXPECT_TRUE(box.alignment() & Qt::AlignRight);
}

TEST(FractionalSpinBox, Typed_fractions_are_kept_and_truncated_by_int_value)
{
    FractionalSpinBox adults;
    adults.set_range(1, 9);
    adults.set_value(1);
    EXPECT_FALSE(type_and_enter(adults, QStringLiteral("2.7")));  // Enter commits and is not consumed
    EXPECT_DOUBLE_EQ(adults.value(), 2.7);
    EXPECT_EQ(adults.text(), QStringLiteral("3"));  // "2.7 adults shows 3, searches 2"
    EXPECT_EQ(adults.int_value(), 2);
}

TEST(FractionalSpinBox, Parses_like_Decimal_Parse_with_the_culture_separators)
{
    FractionalSpinBox box;  // en-US symbols by default
    EXPECT_EQ(box.value_from_text(QStringLiteral("12")), 12.0);
    EXPECT_EQ(box.value_from_text(QStringLiteral("  12.5 ")), 12.5);
    EXPECT_EQ(box.value_from_text(QStringLiteral("1,500")), 1500.0);
    EXPECT_EQ(box.value_from_text(QStringLiteral(".5")), 0.5);
    EXPECT_EQ(box.value_from_text(QStringLiteral("5.")), 5.0);
    EXPECT_EQ(box.value_from_text(QStringLiteral("-5")), -5.0);
    EXPECT_EQ(box.value_from_text(QStringLiteral("5-")), -5.0);  // AllowTrailingSign
    EXPECT_EQ(box.value_from_text(QStringLiteral("+7")), 7.0);
    EXPECT_FALSE(box.value_from_text(QStringLiteral("")));
    EXPECT_FALSE(box.value_from_text(QStringLiteral("abc")));
    EXPECT_FALSE(box.value_from_text(QStringLiteral(",5")));     // a group separator needs digits before it
    EXPECT_FALSE(box.value_from_text(QStringLiteral("1.2.3")));
    EXPECT_FALSE(box.value_from_text(QStringLiteral("-")));
    EXPECT_FALSE(box.value_from_text(QStringLiteral("1 2")));

    box.set_number_symbols(german());
    EXPECT_EQ(box.number_symbols().decimal_separator, ",");
    EXPECT_EQ(box.value_from_text(QStringLiteral("2,7")), 2.7);
    EXPECT_EQ(box.value_from_text(QStringLiteral("1.500")), 1500.0);  // '.' is the de-DE group separator
    EXPECT_EQ(box.value_from_text(QStringLiteral("2.7")), 27.0);      // … as Decimal.Parse reads it

    // A typed space stands for a no-break space in the culture's separator (.NET MatchChars).
    box.set_number_symbols(NumberSymbols{",", " ", "-", {3}});
    EXPECT_EQ(box.value_from_text(QStringLiteral("1 500,25")), 1500.25);
    EXPECT_EQ(box.value_from_text(QString::fromUtf8("1 500")), 1500.0);
}

TEST(FractionalSpinBox, De_DE_typing_uses_the_comma)
{
    FractionalSpinBox radius;
    radius.set_number_symbols(german());
    radius.set_range(0, 1500);
    radius.set_value(250);
    type_and_enter(radius, QStringLiteral("37,5"));
    EXPECT_DOUBLE_EQ(radius.value(), 37.5);
    EXPECT_EQ(radius.text(), QStringLiteral("38"));
    EXPECT_EQ(radius.int_value(), 37);
}

TEST(FractionalSpinBox, Validator_accepts_only_number_characters)
{
    FractionalSpinBox box;
    int position = 0;
    QString text = QStringLiteral("12a");
    EXPECT_EQ(box.validate(text, position), QValidator::Invalid);
    text = QStringLiteral("12.");
    EXPECT_EQ(box.validate(text, position), QValidator::Acceptable);
    text = QStringLiteral("-");
    EXPECT_EQ(box.validate(text, position), QValidator::Intermediate);
    text = QString();
    EXPECT_EQ(box.validate(text, position), QValidator::Intermediate);

    // Typing a letter is rejected by the editor.
    box.set_range(0, 100);
    box.set_value(5);
    editor(box)->selectAll();
    type_text(&box, QStringLiteral("x"));
    EXPECT_EQ(editor(box)->text(), QStringLiteral("5"));
}

TEST(FractionalSpinBox, Commit_clamps_to_the_range_and_reverts_text_that_is_not_a_number)
{
    FractionalSpinBox box;
    box.set_range(0, 1500);
    box.set_value(250);
    std::vector<double> changed;
    QObject::connect(&box, &FractionalSpinBox::valueChanged, [&changed](double value) { changed.push_back(value); });

    type_and_enter(box, QStringLiteral("2000"));
    EXPECT_EQ(box.value(), 1500.0);
    EXPECT_EQ(box.text(), QStringLiteral("1500"));
    type_and_enter(box, QStringLiteral("-3"));
    EXPECT_EQ(box.value(), 0.0);
    ASSERT_EQ(changed.size(), 2u);
    EXPECT_EQ(changed.at(1), 0.0);

    type_and_enter(box, QStringLiteral("-"));  // not a number: the old value comes back
    EXPECT_EQ(box.value(), 0.0);
    EXPECT_EQ(box.text(), QStringLiteral("0"));
    editor(box)->selectAll();
    send_key(&box, Qt::Key_Delete);  // the user empties the box
    box.commit();
    EXPECT_EQ(box.text(), QStringLiteral("0"));
    EXPECT_EQ(changed.size(), 2u);  // nothing changed

    type_and_enter(box, QStringLiteral("0"));  // same value: no signal
    EXPECT_EQ(changed.size(), 2u);
}

TEST(FractionalSpinBox, Typing_alone_changes_nothing_until_the_commit)
{
    FractionalSpinBox box;
    box.set_range(0, 60);
    box.set_value(2);
    std::vector<double> changed;
    QObject::connect(&box, &FractionalSpinBox::valueChanged, [&changed](double value) { changed.push_back(value); });
    editor(box)->selectAll();
    type_text(&box, QStringLiteral("15"));
    EXPECT_EQ(box.value(), 2.0);
    EXPECT_TRUE(changed.empty());
    send_key(&box, Qt::Key_Enter, {}, Qt::KeypadModifier);
    EXPECT_EQ(box.value(), 15.0);
    EXPECT_EQ(changed.size(), 1u);
}

TEST(FractionalSpinBox, Steps_keep_the_fraction_and_stop_at_the_bounds)
{
    FractionalSpinBox radius;
    radius.set_range(0, 1500);
    radius.set_single_step(25);
    EXPECT_EQ(radius.single_step(), 25.0);
    radius.set_value(2.5);
    radius.stepBy(1);
    EXPECT_DOUBLE_EQ(radius.value(), 27.5);  // typed values need not be multiples of 25
    EXPECT_EQ(radius.text(), QStringLiteral("28"));
    radius.stepBy(-2);
    EXPECT_EQ(radius.value(), 0.0);
    radius.set_value(1490);
    radius.stepBy(1);
    EXPECT_EQ(radius.value(), 1500.0);

    // Up/Down keys step too, after committing pending text first.
    radius.set_value(100);
    editor(radius)->selectAll();
    type_text(&radius, QStringLiteral("110"));
    send_key(&radius, Qt::Key_Up);
    EXPECT_EQ(radius.value(), 135.0);
}

TEST(FractionalSpinBox, Only_user_edits_are_parsed_so_the_fraction_survives_focus_changes)
{
    FractionalSpinBox radius;
    radius.set_range(0, 1500);
    type_and_enter(radius, QStringLiteral("37.5"));
    EXPECT_FALSE(radius.user_edit());
    Q_EMIT radius.editingFinished();  // focus loss without typing: the text "38" is not re-read
    radius.commit();
    EXPECT_DOUBLE_EQ(radius.value(), 37.5);

    editor(radius)->selectAll();
    type_text(&radius, QStringLiteral("4"));
    EXPECT_TRUE(radius.user_edit());
    Q_EMIT radius.editingFinished();  // focus loss after typing commits
    EXPECT_EQ(radius.value(), 4.0);
    EXPECT_FALSE(radius.user_edit());
}

TEST(FractionalSpinBox, Range_clamps_the_current_value_and_ignores_NaN)
{
    FractionalSpinBox box;
    box.set_range(0, 120);
    box.set_value(100);
    box.set_range(0, 60);
    EXPECT_EQ(box.value(), 60.0);
    EXPECT_EQ(box.minimum(), 0.0);
    EXPECT_EQ(box.maximum(), 60.0);
    box.set_value(std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(box.value(), 60.0);
}

} // namespace
