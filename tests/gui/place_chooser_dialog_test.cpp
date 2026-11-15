// PlaceChooserDialog: 01 §11.5 (texts, layout, default / cancel buttons,
// double-click = OK, Esc = Cancel), CT-GEOC-04 style candidates.

#include "place_chooser_dialog.hpp"

#include "input_test_support.hpp"
#include "ui_style.hpp"

#include "wizz/core/domain.hpp"

#include <QApplication>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

namespace {

using wizz::Place;
using wizz::app::PlaceChooserDialog;
using namespace wizz::test::gui;

[[nodiscard]] Place place(std::string name, std::string description)
{
    Place result;
    result.name = std::move(name);
    result.description = std::move(description);
    result.source = wizz::PlaceSource::geocoder;
    return result;
}

[[nodiscard]] std::vector<Place> londons()
{
    return {place("London", "London, United Kingdom"), place("London", "London, Ontario, Canada"),
            place("London", "London (from “Londn”)")};
}

[[nodiscard]] PlaceChooserDialog* visible_chooser()
{
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* dialog = qobject_cast<PlaceChooserDialog*>(widget); dialog != nullptr && dialog->isVisible()) {
            return dialog;
        }
    }
    return nullptr;
}

/// Runs `action` on the chooser once its modal loop runs (and rejects it after a
/// while if the action did not close it, so a failing test cannot hang).
void when_shown(std::function<void(PlaceChooserDialog&)> action)
{
    QTimer::singleShot(0, [action = std::move(action)] {
        if (PlaceChooserDialog* dialog = visible_chooser()) {
            action(*dialog);
        }
    });
    QTimer::singleShot(5000, [] {
        if (PlaceChooserDialog* dialog = visible_chooser()) {
            dialog->reject();
        }
    });
}

TEST(PlaceChooserDialog, One_candidate_asks_about_this_place)
{
    QWidget owner;
    const std::vector<Place> candidates{place("Vienna", "Vienna, Austria")};
    PlaceChooserDialog dialog("Wien", "starting point", candidates, std::nullopt, &owner);

    EXPECT_EQ(dialog.windowTitle(), QStringLiteral("Which starting point?"));
    EXPECT_EQ(dialog.intro_label()->text().toStdString(), "Did you mean this place for “Wien”?");
    EXPECT_TRUE(dialog.intro_label()->wordWrap());
    ASSERT_EQ(dialog.list()->count(), 1);
    EXPECT_EQ(dialog.list()->item(0)->text(), QStringLiteral("Vienna, Austria"));
    EXPECT_EQ(dialog.selected_index(), 0u);
    EXPECT_EQ(dialog.selected_place(), candidates[0]);
    EXPECT_EQ(dialog.ok_button()->text(), QStringLiteral("Use this place"));
    EXPECT_EQ(dialog.cancel_button()->text(), QStringLiteral("Cancel"));
    EXPECT_TRUE(dialog.ok_button()->isDefault());
    EXPECT_FALSE(dialog.cancel_button()->isDefault());
    EXPECT_FALSE(dialog.chosen_index());  // not accepted yet
}

TEST(PlaceChooserDialog, Is_a_fixed_560_by_280_modal_dialog_owned_by_the_window)
{
    QWidget owner;
    PlaceChooserDialog dialog("Londn", "destination", londons(), std::nullopt, &owner);
    EXPECT_EQ(dialog.size(), QSize(560, 280));
    EXPECT_EQ(dialog.minimumSize(), QSize(560, 280));
    EXPECT_EQ(dialog.maximumSize(), QSize(560, 280));
    EXPECT_TRUE(dialog.isModal());
    EXPECT_FALSE(dialog.isSizeGripEnabled());
    // Owned by the main window (not in the taskbar); no minimise / maximise / help.
    EXPECT_EQ(dialog.parentWidget(), &owner);
    EXPECT_TRUE(dialog.isWindow());
    const Qt::WindowFlags flags = dialog.windowFlags();
    EXPECT_FALSE(flags.testFlag(Qt::WindowMinimizeButtonHint));
    EXPECT_FALSE(flags.testFlag(Qt::WindowMaximizeButtonHint));
    EXPECT_FALSE(flags.testFlag(Qt::WindowContextHelpButtonHint));
    EXPECT_TRUE(flags.testFlag(Qt::WindowCloseButtonHint));
    EXPECT_EQ(dialog.layout()->contentsMargins(), QMargins(10, 10, 10, 10));
}

TEST(PlaceChooserDialog, Several_candidates_with_a_hint)
{
    const std::vector<Place> candidates = londons();
    const std::string hint = "“STN” is not a Wizz Air airport code, so it was looked up as a place name.";
    PlaceChooserDialog dialog("Londn", "destination", candidates, hint);
    EXPECT_EQ(dialog.windowTitle(), QStringLiteral("Which destination?"));
    EXPECT_EQ(dialog.intro_label()->text().toStdString(),
              hint + "\nDid you mean one of these places for “Londn”? Pick the destination:");
    ASSERT_EQ(dialog.list()->count(), 3);
    EXPECT_EQ(dialog.list()->item(1)->text(), QStringLiteral("London, Ontario, Canada"));
    EXPECT_EQ(dialog.list()->item(2)->text().toStdString(), "London (from “Londn”)");
    EXPECT_EQ(dialog.selected_index(), 0u);  // the first row is selected

    dialog.list()->setCurrentRow(1);
    dialog.ok_button()->click();
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
    EXPECT_EQ(dialog.chosen_index(), 1u);
    EXPECT_EQ(dialog.selected_place(), candidates[1]);
}

TEST(PlaceChooserDialog, Layout_puts_the_intro_on_top_and_Cancel_rightmost)
{
    PlaceChooserDialog dialog("Londn", "destination", londons(), std::nullopt);
    dialog.setFont(wizz::app::style::form_font());
    dialog.setAttribute(Qt::WA_DontShowOnScreen);
    dialog.show();
    dialog.layout()->activate();

    const QRect intro = dialog.intro_label()->geometry();
    const QRect list = dialog.list()->geometry();
    const QRect ok = dialog.ok_button()->geometry();
    const QRect cancel = dialog.cancel_button()->geometry();
    EXPECT_EQ(intro.topLeft(), QPoint(10, 10));
    EXPECT_EQ(intro.width(), 560 - 20);  // wraps at client width − 20
    EXPECT_EQ(list.x(), 10);
    EXPECT_EQ(list.width(), 540);
    EXPECT_EQ(list.top(), intro.bottom() + 1);    // the 6 px are inside the intro's bottom padding
    EXPECT_EQ(dialog.intro_label()->contentsMargins().bottom(), 6);
    EXPECT_GE(ok.top(), list.bottom() + 1 + 8);   // buttons 8 px below the list
    EXPECT_EQ(ok.top(), cancel.top());
    EXPECT_GT(cancel.left(), ok.right());         // Cancel is rightmost
    EXPECT_EQ(cancel.right(), 560 - 10 - 1);      // right-aligned at the padding
    EXPECT_EQ(cancel.bottom(), 280 - 10 - 1);
    save_render(dialog, QStringLiteral("place-chooser"));
    dialog.reject();
}

TEST(PlaceChooserDialog, Double_click_on_a_row_is_OK)
{
    PlaceChooserDialog dialog("Londn", "destination", londons(), std::nullopt);
    dialog.setAttribute(Qt::WA_DontShowOnScreen);
    dialog.show();
    QListWidget* list = dialog.list();
    const QRect row = list->visualItemRect(list->item(2));
    ASSERT_TRUE(row.isValid());
    send_double_click(list->viewport(), row.center());
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
    EXPECT_FALSE(dialog.isVisible());
    EXPECT_EQ(dialog.chosen_index(), 2u);
}

TEST(PlaceChooserDialog, Esc_Cancel_and_the_close_box_give_nothing)
{
    {
        PlaceChooserDialog dialog("Londn", "destination", londons(), std::nullopt);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        send_key(&dialog, Qt::Key_Escape);  // Esc = Cancel (D-11)
        EXPECT_EQ(dialog.result(), QDialog::Rejected);
        EXPECT_FALSE(dialog.isVisible());
        EXPECT_FALSE(dialog.chosen_index());
    }
    {
        PlaceChooserDialog dialog("Londn", "destination", londons(), std::nullopt);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        dialog.cancel_button()->click();
        EXPECT_EQ(dialog.result(), QDialog::Rejected);
        EXPECT_FALSE(dialog.chosen_index());
    }
    {
        PlaceChooserDialog dialog("Londn", "destination", londons(), std::nullopt);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        dialog.close();
        EXPECT_EQ(dialog.result(), QDialog::Rejected);
        EXPECT_FALSE(dialog.chosen_index());
    }
    {
        // Enter presses the default button.
        PlaceChooserDialog dialog("Londn", "destination", londons(), std::nullopt);
        dialog.setAttribute(Qt::WA_DontShowOnScreen);
        dialog.show();
        send_key(dialog.list(), Qt::Key_Return);
        EXPECT_EQ(dialog.result(), QDialog::Accepted);
        EXPECT_EQ(dialog.chosen_index(), 0u);
    }
}

TEST(PlaceChooserDialog, Choose_runs_modally_and_returns_the_chosen_place)
{
    QWidget owner;
    const std::vector<Place> candidates = londons();

    when_shown([](PlaceChooserDialog& dialog) {
        dialog.list()->setCurrentRow(1);
        dialog.ok_button()->click();
    });
    EXPECT_EQ(PlaceChooserDialog::choose(&owner, "Londn", "destination", candidates, std::nullopt), candidates[1]);

    when_shown([](PlaceChooserDialog& dialog) { dialog.ok_button()->click(); });
    EXPECT_EQ(PlaceChooserDialog::choose_index(&owner, "Londn", "destination", candidates, std::nullopt), 0u);

    when_shown([](PlaceChooserDialog& dialog) { dialog.cancel_button()->click(); });
    EXPECT_EQ(PlaceChooserDialog::choose(&owner, "Londn", "destination", candidates, std::nullopt), std::nullopt);

    when_shown([](PlaceChooserDialog& dialog) { send_key(&dialog, Qt::Key_Escape); });
    EXPECT_EQ(PlaceChooserDialog::choose_index(&owner, "Londn", "destination", candidates, std::nullopt),
              std::nullopt);
}

} // namespace
