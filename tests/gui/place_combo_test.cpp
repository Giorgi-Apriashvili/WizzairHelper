// PlaceCombo: editable place box with SuggestAppend autocomplete (01 §4.4, §11.2).

#include "place_combo.hpp"

#include "input_test_support.hpp"

#include <QApplication>
#include <QCompleter>
#include <QLineEdit>

#include <gtest/gtest.h>

namespace {

using wizz::app::PlaceCombo;
using namespace wizz::test::gui;

[[nodiscard]] QStringList sample_names()
{
    return {QStringLiteral("Barcelona"), QStringLiteral("Bratislava"), QStringLiteral("Budapest"),
            QStringLiteral("London"), QStringLiteral("London Luton")};
}

TEST(PlaceCombo, Is_an_editable_box_with_a_20_item_drop_down_that_never_inserts_typed_text)
{
    PlaceCombo box;
    EXPECT_TRUE(box.isEditable());
    EXPECT_EQ(box.maxVisibleItems(), 20);
    EXPECT_EQ(box.insertPolicy(), QComboBox::NoInsert);
    ASSERT_NE(box.completer(), nullptr);
    EXPECT_EQ(box.completer()->completionMode(), QCompleter::PopupCompletion);
    EXPECT_EQ(box.completer()->caseSensitivity(), Qt::CaseInsensitive);
    EXPECT_EQ(box.completer()->filterMode(), Qt::MatchStartsWith);
    EXPECT_EQ(box.completer()->model(), box.model());  // the drop-down items are the source

    box.set_suggestions(sample_names());
    box.set_text(QStringLiteral("Graz"));
    send_key(&box, Qt::Key_Return);
    EXPECT_EQ(box.count(), 5);
    EXPECT_EQ(box.text(), QStringLiteral("Graz"));
}

TEST(PlaceCombo, Suggestions_replace_the_items_and_keep_the_typed_text)
{
    PlaceCombo box;
    box.set_text(QStringLiteral("Budapest"));
    box.set_suggestions(sample_names());
    EXPECT_EQ(box.suggestions(), sample_names());
    EXPECT_EQ(box.text(), QStringLiteral("Budapest"));
    box.set_text(QStringLiteral("  wien "));
    box.set_suggestions({QStringLiteral("Vienna")});
    EXPECT_EQ(box.count(), 1);
    EXPECT_EQ(box.text(), QStringLiteral("  wien "));
    box.set_text(QString());
    box.set_suggestions(sample_names());
    EXPECT_EQ(box.text(), QString());  // an empty "To" stays empty (anywhere)
}

TEST(PlaceCombo, Append_completion_is_the_rest_of_the_first_case_insensitive_match)
{
    PlaceCombo box;
    box.set_suggestions(sample_names());
    EXPECT_EQ(box.append_completion(QStringLiteral("bud")), QStringLiteral("apest"));
    EXPECT_EQ(box.append_completion(QStringLiteral("LON")), QStringLiteral("don"));
    EXPECT_EQ(box.append_completion(QStringLiteral("London ")), QStringLiteral("Luton"));
    EXPECT_EQ(box.append_completion(QStringLiteral("London")), QStringLiteral(" Luton"));  // longer match
    EXPECT_EQ(box.append_completion(QStringLiteral("Graz")), QString());
    EXPECT_EQ(box.append_completion(QString()), QString());
}

TEST(PlaceCombo, Typing_appends_the_suggestion_selected_and_Backspace_removes_it)
{
    PlaceCombo box;
    box.set_suggestions(sample_names());
    box.set_text(QString());
    type_text(&box, QStringLiteral("bud"));
    QApplication::processEvents();
    QLineEdit* edit = box.lineEdit();
    EXPECT_EQ(edit->text(), QStringLiteral("budapest"));
    EXPECT_EQ(edit->selectedText(), QStringLiteral("apest"));

    send_key(&box, Qt::Key_Backspace);  // removes the appended part only
    QApplication::processEvents();
    EXPECT_EQ(edit->text(), QStringLiteral("bud"));
    EXPECT_FALSE(edit->hasSelectedText());

    type_text(&box, QStringLiteral("x"));  // no match: nothing appended
    QApplication::processEvents();
    EXPECT_EQ(edit->text(), QStringLiteral("budx"));
}

} // namespace
