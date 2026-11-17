// The Settings button and the Settings window (D-59..D-63): the gear at the right end of
// the tab row (no height of its own, its place in the Tab order, its look), the window
// (modal, Esc and Close, the Tab order and the names screen readers read), what it shows
// for every state of the updater (live while a check or download runs), each change
// applied at once and saved to settings.json (never in autotest), each install choice end
// to end from the window to the update bar and closing or restarting the app, a portable
// copy, a run without updates, and its layout and colours in light, dark and High
// Contrast. The updater is the scripted FakeUpdateBackend on a fixed clock; downloads are
// held and released by a gate, never by timing.

#include "update_test_support.hpp"

#include "autotest_runner.hpp"
#include "grid_table_view.hpp"
#include "layout_check.hpp"
#include "look_test_support.hpp"
#include "minimum_window.hpp"
#include "qt_strings.hpp"
#include "search_button.hpp"
#include "section_card.hpp"
#include "settings_button.hpp"
#include "settings_dialog.hpp"
#include "ui_style.hpp"

#include "wizz/services/app_paths.hpp"
#include "wizz/ui/settings.hpp"
#include "wizz/ui/update_model.hpp"

#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QStyleFactory>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QtMath>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::app::MainWindow;
using wizz::app::SettingsButton;
using wizz::app::SettingsDialog;
using wizz::test::app::release_info;
using wizz::test::app::update_error;
using wizz::test::app::UpdateRig;
using wizz::test::app::UpdateRigOptions;
using wizz::test::app::WindowRig;
using wizz::test::app::WindowRigOptions;
namespace gui = wizz::test::gui;
namespace style = wizz::app::style;
namespace update = wizz::update;
namespace strings = wizz::ui::update_strings;
using update::CheckFrequency;
using update::InstallKind;
using update::UpdateErrorKind;
using update::Version;
using update::WhenFound;
using wizz::ui::UpdateStatusKind;
using wizz::ui::UpdateStatusView;

const Version kNewVersion{1, 2, 0};

[[nodiscard]] QString q(std::string_view text)
{
    return wizz::app::to_qstring(text);
}

[[nodiscard]] update::UpdateOptions options(WhenFound when_found, bool automatic = true,
                                            CheckFrequency frequency = CheckFrequency::daily)
{
    return update::UpdateOptions{.check_automatically = automatic, .check_frequency = frequency, .when_found = when_found};
}

[[nodiscard]] UpdateRigOptions rig_options(update::UpdateOptions updates = {}, InstallKind kind = InstallKind::installed)
{
    UpdateRigOptions result;
    result.kind = kind;
    result.window.settings.updates = updates;
    return result;
}

[[nodiscard]] std::filesystem::path settings_path(const WindowRig& rig)
{
    return wizz::app_paths::settings_file(rig.data_root());
}

/// settings.json as the window saved it.
[[nodiscard]] wizz::ui::AppSettings saved(const WindowRig& rig)
{
    EXPECT_TRUE(std::filesystem::exists(settings_path(rig))) << "settings.json was not written";
    return wizz::ui::AppSettings::load(settings_path(rig));
}

/// The Settings button pressed as a user presses it; the window it opened.
[[nodiscard]] SettingsDialog& open_settings(MainWindow& window)
{
    window.settings_button()->click();
    gui::settle();
    SettingsDialog* dialog = window.settings_dialog();
    if (dialog == nullptr) {
        throw std::runtime_error("the Settings button opened no window");
    }
    return *dialog;
}

/// Waits until the controller has no work running and no manual check open.
void wait_idle(UpdateRig& rig)
{
    ASSERT_TRUE(gui::wait_until([&] { return !rig.controller()->is_running() && !rig.controller()->model().checking(); }));
    gui::settle();
}

/// Presses Tab (or Shift+Tab) `count` times in `dialog`; the widgets the focus visited.
[[nodiscard]] std::vector<QWidget*> press_tab(SettingsDialog& dialog, int count, bool back = false)
{
    std::vector<QWidget*> visited;
    for (int i = 0; i < count; ++i) {
        gui::send_key(dialog.focusWidget(), QEvent::KeyPress, back ? Qt::Key_Backtab : Qt::Key_Tab,
                      back ? Qt::ShiftModifier : Qt::NoModifier);
        visited.push_back(dialog.focusWidget());
    }
    return visited;
}

/// Pixels of `image` within `tolerance` of `colour` in every channel.
[[nodiscard]] int pixels_near(const QImage& image, const QRect& area, const QColor& colour, int tolerance)
{
    int count = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            count += std::abs(pixel.red() - colour.red()) <= tolerance &&
                             std::abs(pixel.green() - colour.green()) <= tolerance &&
                             std::abs(pixel.blue() - colour.blue()) <= tolerance
                         ? 1
                         : 0;
        }
    }
    return count;
}

[[nodiscard]] QColor text_colour(const QWidget* widget)
{
    return widget->palette().color(QPalette::WindowText);
}

// ---------------------------------------------------------------------------
// The gear at the end of the tab row
// ---------------------------------------------------------------------------

TEST(SettingsButton, Sits_at_the_right_end_of_the_tab_row_and_costs_no_height)
{
    WindowRig rig;
    rig.start();
    MainWindow& window = rig.window();
    QTabWidget& tabs = *window.tabs();
    const QTabBar& bar = *tabs.tabBar();
    SettingsButton* button = window.settings_button();
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(tabs.cornerWidget(Qt::TopRightCorner), button);
    EXPECT_EQ(button->parentWidget(), &tabs);
    ASSERT_TRUE(button->isVisible());
    EXPECT_EQ(button->text(), "Settings");
    EXPECT_EQ(button->accessibleName(), "Settings");
    EXPECT_EQ(button->toolTip(), "Settings");
    EXPECT_TRUE(button->isFlat());
    EXPECT_EQ(button->focusPolicy(), Qt::StrongFocus);

    // In the tab row, at its right end, as tall as the tabs, clear of the last tab.
    gui::settle();
    const QRect geometry = button->geometry();
    EXPECT_EQ(geometry.top(), bar.geometry().top());
    EXPECT_EQ(geometry.height(), bar.height()) << "exactly the tab row";
    EXPECT_EQ(button->sizeHint().height(), bar.sizeHint().height()) << "asks for no more than the tab bar";
    EXPECT_EQ(geometry.right(), tabs.width() - 1) << "the right end of the row";
    const QRect last_tab{bar.mapTo(&tabs, bar.tabRect(bar.count() - 1).topLeft()), bar.tabRect(bar.count() - 1).size()};
    EXPECT_GT(geometry.left(), last_tab.right());
    EXPECT_EQ(wizz::app::layout_problems(window), std::vector<std::string>{});

    // Without the button the page starts at the same height and the tab widget asks for the
    // same sizes: the gear costs the results nothing.
    const int page_top = tabs.currentWidget()->mapTo(&tabs, QPoint(0, 0)).y();
    const QSize minimum = tabs.minimumSizeHint();
    const QSize hint = tabs.sizeHint();
    tabs.setCornerWidget(nullptr, Qt::TopRightCorner);
    gui::settle();
    EXPECT_EQ(tabs.currentWidget()->mapTo(&tabs, QPoint(0, 0)).y(), page_top);
    EXPECT_EQ(tabs.minimumSizeHint().height(), minimum.height());
    EXPECT_EQ(tabs.sizeHint().height(), hint.height());
    tabs.setCornerWidget(button, Qt::TopRightCorner);
    button->show();
    gui::settle();
    EXPECT_EQ(button->geometry(), geometry);
}

TEST(SettingsButton, Paints_the_gear_in_the_accent_and_its_text_like_a_tab)
{
    WindowRig rig;
    rig.start();
    SettingsButton& button = *rig.window().settings_button();
    const style::Theme& theme = style::theme();
    EXPECT_EQ(button.glyph_colour(), theme.accent_line) << "like the card icons";
    EXPECT_EQ(button.text_colour(), theme.text_secondary) << "like an unselected tab";
    EXPECT_EQ(button.font().families(), style::form_font().families());

    const QImage image = button.grab().toImage();
    ASSERT_FALSE(image.isNull());
    const int glyph_end = SettingsButton::kPadding.left() +
                          qCeil(style::icon_size(style::glyph::kSettings, SettingsButton::kGlyphPointSize).width());
    const QRect glyph_area{0, 0, glyph_end + 1, image.height()};
    const QRect text_area{glyph_end + SettingsButton::kGlyphGap, 0, image.width() - glyph_end - SettingsButton::kGlyphGap,
                          image.height()};
    EXPECT_GE(pixels_near(image, glyph_area, theme.accent_line, 40), 6) << "the gear is drawn in the accent";
    EXPECT_EQ(pixels_near(image, text_area, theme.accent_line, 40), 0) << "the text is not";
    EXPECT_GE(pixels_near(image, text_area, theme.text_secondary, 40), 6) << "the text in the secondary colour";

    button.setAttribute(Qt::WA_UnderMouse);
    EXPECT_EQ(button.text_colour(), theme.text) << "under the mouse, like a hovered tab";
    button.setAttribute(Qt::WA_UnderMouse, false);
}

TEST(SettingsButton, Follows_the_result_tabs_in_the_Tab_order_and_opens_with_the_keyboard)
{
    UpdateRig rig;
    rig.start();
    MainWindow& window = rig.window();
    QTabBar* bar = window.tabs()->tabBar();
    SettingsButton* button = window.settings_button();
    const std::vector<QWidget*> stops = gui::tab_stops(window);
    const auto at = [&stops](const QWidget* widget) { return std::ranges::find(stops, widget) - stops.begin(); };
    ASSERT_LT(at(button), static_cast<std::ptrdiff_t>(stops.size()));
    EXPECT_EQ(at(button), at(bar) + 1) << "at the end of the tab row";
    const QWidget* first_on_page = rig.window_rig.outbound().table();
    EXPECT_EQ(at(first_on_page), at(button) + 1) << "then the page";

    bar->setFocus(Qt::TabFocusReason);
    gui::send_key(bar, QEvent::KeyPress, Qt::Key_Tab);
    EXPECT_EQ(QApplication::focusWidget(), button);
    gui::send_key(button, QEvent::KeyPress, Qt::Key_Return);  // the window clicks a focused button
    gui::settle();
    ASSERT_NE(window.settings_dialog(), nullptr) << "Enter opened the Settings window";
    EXPECT_TRUE(window.settings_dialog()->isVisible());
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

TEST(SettingsWindow, Enter_clicks_the_focused_button_and_otherwise_Close)
{
    UpdateRig rig;
    rig.start();
    SettingsDialog& dialog = open_settings(rig.window());
    ASSERT_EQ(dialog.focusWidget(), dialog.check_now_button());
    gui::send_key(dialog.check_now_button(), QEvent::KeyPress, Qt::Key_Return);
    wait_idle(rig);
    EXPECT_EQ(rig.backend->manual_checks(), 1) << "Enter on Check now checks";
    ASSERT_EQ(rig.window().settings_dialog(), &dialog) << "and leaves the window open";
    EXPECT_TRUE(dialog.isVisible());
    EXPECT_EQ(dialog.focusWidget(), dialog.check_now_button());

    // On a control that is not a push button, Enter clicks the default button, Close.
    dialog.check_automatically_box()->setFocus(Qt::TabFocusReason);
    gui::send_key(dialog.check_automatically_box(), QEvent::KeyPress, Qt::Key_Enter, Qt::KeypadModifier);
    ASSERT_TRUE(gui::wait_until([&] { return rig.window().settings_dialog() == nullptr; }));
    EXPECT_TRUE(rig.window().isVisible());
    EXPECT_TRUE(rig.controller()->options().check_automatically) << "the check box was not toggled";
    EXPECT_EQ(rig.backend->manual_checks(), 1);
}

TEST(SettingsWindow, Check_now_keeps_the_keyboard_focus_through_its_check)
{
    // Disabled while it runs, Check now must not pass the focus to the check box: a second
    // Space (to check again) would turn automatic checks off and save that.
    UpdateRig rig;
    rig.start();
    SettingsDialog& dialog = open_settings(rig.window());
    QPushButton* check_now = dialog.check_now_button();
    ASSERT_EQ(dialog.focusWidget(), check_now);
    gui::tap_key(check_now, Qt::Key_Space);
    EXPECT_FALSE(check_now->isEnabled()) << "busy";
    EXPECT_EQ(dialog.focusWidget(), dialog.status_label()) << "the focus waits on the status line";
    wait_idle(rig);
    EXPECT_EQ(rig.backend->manual_checks(), 1);
    EXPECT_EQ(dialog.focusWidget(), check_now) << "and comes back with Check now";

    gui::tap_key(dialog.focusWidget(), Qt::Key_Space);
    wait_idle(rig);
    EXPECT_EQ(rig.backend->manual_checks(), 2) << "Space again checks again";
    EXPECT_TRUE(dialog.check_automatically_box()->isChecked()) << "automatic checks untouched";
    EXPECT_TRUE(rig.controller()->options().check_automatically);
    EXPECT_FALSE(std::filesystem::exists(settings_path(rig.window_rig))) << "no option changed, nothing saved";

    // A download keeps it disabled longer; the focus still comes back when it ends.
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.backend->hold_downloads();
    gui::tap_key(check_now, Qt::Key_Space);
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->downloads_started() == 1; }));
    gui::settle();
    EXPECT_EQ(dialog.status().kind, UpdateStatusKind::downloading);
    EXPECT_EQ(dialog.focusWidget(), dialog.status_label());
    rig.backend->release_downloads();
    wait_idle(rig);
    ASSERT_EQ(dialog.status().kind, UpdateStatusKind::ready);
    EXPECT_EQ(dialog.focusWidget(), check_now);

    // Moved away meanwhile, the focus stays where the user put it.
    rig.clock->advance(std::chrono::minutes{1});
    rig.backend->script_checks({std::unexpected(update_error(UpdateErrorKind::network, "no route to host"))});
    gui::tap_key(check_now, Qt::Key_Space);
    EXPECT_EQ(dialog.focusWidget(), dialog.status_label());
    EXPECT_EQ(press_tab(dialog, 1), (std::vector<QWidget*>{dialog.check_automatically_box()}))
        << "Tab moves on from the status line";
    wait_idle(rig);
    EXPECT_TRUE(check_now->isEnabled());
    EXPECT_EQ(dialog.focusWidget(), dialog.check_automatically_box());
}

TEST(SettingsWindow, Is_a_fixed_modal_window_of_the_main_window_and_closes_with_Close_or_Esc)
{
    UpdateRig rig;
    rig.start();
    MainWindow& window = rig.window();
    SettingsDialog& dialog = open_settings(window);
    EXPECT_EQ(dialog.windowTitle(), "Settings");
    EXPECT_EQ(dialog.parentWidget(), &window) << "owned: no taskbar button, centred on the window";
    EXPECT_TRUE(dialog.isModal());
    EXPECT_EQ(dialog.windowModality(), Qt::WindowModal);
    EXPECT_FALSE(dialog.windowFlags().testFlag(Qt::WindowMinimizeButtonHint));
    EXPECT_FALSE(dialog.windowFlags().testFlag(Qt::WindowMaximizeButtonHint));
    EXPECT_FALSE(dialog.windowFlags().testFlag(Qt::WindowContextHelpButtonHint));
    EXPECT_EQ(dialog.minimumSize(), dialog.maximumSize()) << "a fixed size";
    EXPECT_EQ(dialog.width(), SettingsDialog::kClientWidth);
    EXPECT_TRUE(dialog.close_button()->isDefault()) << "the primary button";
    EXPECT_EQ(dialog.close_button()->text(), "Close");
    EXPECT_EQ(window.open_settings(), &dialog) << "a second press raises the open window";

    gui::send_key(dialog.check_automatically_box(), QEvent::KeyPress, Qt::Key_Escape);
    ASSERT_TRUE(gui::wait_until([&] { return window.settings_dialog() == nullptr; })) << "Esc closes it";
    EXPECT_TRUE(window.isVisible()) << "and nothing else";
    EXPECT_FALSE(rig.window_rig.controller().is_working());

    SettingsDialog& again = open_settings(window);
    again.close_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return window.settings_dialog() == nullptr; })) << "Close closes it";
}

TEST(SettingsWindow, Shows_the_saved_options_and_saves_every_change_at_once)
{
    UpdateRigOptions setup = rig_options(options(WhenFound::download_then_ask, true, CheckFrequency::weekly));
    setup.window.settings.from = "Wien";
    UpdateRig rig{std::move(setup)};
    rig.start();
    SettingsDialog& dialog = open_settings(rig.window());

    EXPECT_TRUE(dialog.check_automatically_box()->isChecked());
    EXPECT_EQ(dialog.frequency_box()->currentText(), "Once a week");
    EXPECT_TRUE(dialog.frequency_box()->isEnabled());
    EXPECT_TRUE(dialog.when_found_button(WhenFound::download_then_ask)->isChecked());
    EXPECT_FALSE(dialog.when_found_button(WhenFound::install_on_close)->isChecked());
    EXPECT_FALSE(dialog.when_found_button(WhenFound::notify_only)->isChecked());
    EXPECT_EQ(dialog.options(), options(WhenFound::download_then_ask, true, CheckFrequency::weekly));
    std::vector<QString> items;
    for (int i = 0; i < dialog.frequency_box()->count(); ++i) {
        items.push_back(dialog.frequency_box()->itemText(i));
    }
    EXPECT_EQ(items, (std::vector<QString>{"Once a day", "Once a week", "Once a month"}));
    EXPECT_FALSE(std::filesystem::exists(settings_path(rig.window_rig))) << "opening saves nothing";

    // Each change applies to the updater and is in settings.json at once.
    dialog.frequency_box()->setCurrentIndex(2);
    EXPECT_EQ(saved(rig.window_rig).updates, options(WhenFound::download_then_ask, true, CheckFrequency::monthly));
    EXPECT_EQ(rig.controller()->options().check_frequency, CheckFrequency::monthly);

    dialog.when_found_button(WhenFound::notify_only)->click();
    EXPECT_EQ(saved(rig.window_rig).updates, options(WhenFound::notify_only, true, CheckFrequency::monthly));
    EXPECT_EQ(rig.controller()->options().when_found, WhenFound::notify_only);

    dialog.check_automatically_box()->click();
    EXPECT_FALSE(dialog.frequency_box()->isEnabled()) << "how often means nothing while it is off";
    EXPECT_EQ(saved(rig.window_rig).updates, options(WhenFound::notify_only, false, CheckFrequency::monthly));
    EXPECT_FALSE(rig.controller()->options().check_automatically);
    EXPECT_EQ(rig.window().current_settings().updates, options(WhenFound::notify_only, false, CheckFrequency::monthly));
    EXPECT_EQ(saved(rig.window_rig).from, "Wien") << "the other settings are saved as they are";

    // Opened again, the window shows what was saved; closing the app saves it too.
    dialog.close();
    ASSERT_TRUE(gui::wait_until([&] { return rig.window().settings_dialog() == nullptr; }));
    SettingsDialog& again = open_settings(rig.window());
    EXPECT_EQ(again.options(), options(WhenFound::notify_only, false, CheckFrequency::monthly));
    rig.window().close();
    EXPECT_EQ(saved(rig.window_rig).updates, options(WhenFound::notify_only, false, CheckFrequency::monthly));
}

TEST(SettingsWindow, Unchecking_the_box_stops_the_automatic_checks_but_not_Check_now)
{
    UpdateRigOptions setup = rig_options();
    setup.interval = 20ms;
    UpdateRig rig{std::move(setup)};
    rig.start();
    ASSERT_EQ(rig.backend->checks(), 1) << "a fresh profile checks once";
    SettingsDialog& dialog = open_settings(rig.window());
    dialog.check_automatically_box()->click();
    // An evaluation already on its worker carries the policy from before the click; let it
    // end before the clock moves, so the new period meets only the switched-off policy.
    ASSERT_TRUE(gui::wait_until([&] { return !rig.controller()->is_running(); }));
    rig.clock->advance(std::chrono::days{40});  // a new day, week and month
    const int evaluations = rig.backend->evaluations();
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->evaluations() >= evaluations + 3; }));
    EXPECT_EQ(rig.backend->checks(), 1) << "no request to GitHub while it is off";

    dialog.check_now_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->manual_checks() == 1 && !rig.controller()->model().checking(); }));
    EXPECT_EQ(rig.backend->checks(), 2) << "Check now still checks";

    dialog.check_automatically_box()->click();  // on again: this period was just checked
    const int later = rig.backend->evaluations();
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->evaluations() >= later + 3; }));
    EXPECT_EQ(rig.backend->checks(), 2);
    rig.clock->advance(std::chrono::days{1});
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->checks() == 3; })) << "the next day checks again";
}

TEST(SettingsWindow, Check_now_shows_checking_then_each_result_live_like_the_update_bar)
{
    UpdateRig rig;
    rig.start();  // releases/latest answers 404: up to date at 10:00
    SettingsDialog& dialog = open_settings(rig.window());
    const style::Theme& theme = style::theme();
    EXPECT_EQ(dialog.version_label()->text(), q("WizzairHelper 1.0.0 · last checked today at 10:00"));
    EXPECT_EQ(dialog.status_label()->text(), "You have the latest version.");
    EXPECT_EQ(text_colour(dialog.status_label()), theme.success);
    EXPECT_TRUE(dialog.check_now_button()->isEnabled());
    EXPECT_EQ(dialog.check_now_button()->text(), "Check now");
    const int width = dialog.check_now_button()->width();
    const QSize size = dialog.size();

    // A failed check: busy, then the failure in plain words.
    rig.clock->advance(std::chrono::minutes{5});
    rig.backend->script_checks({std::unexpected(update_error(UpdateErrorKind::network,
                                                             "Update check failed: network error: no route to host"))});
    dialog.check_now_button()->click();
    EXPECT_FALSE(dialog.check_now_button()->isEnabled()) << "busy at once";
    EXPECT_EQ(dialog.check_now_button()->text(), q("Checking…"));
    EXPECT_EQ(dialog.status_label()->text(), q("Checking for updates…"));
    EXPECT_EQ(text_colour(dialog.status_label()), theme.text_secondary);
    gui::settle();
    EXPECT_EQ(dialog.check_now_button()->width(), width) << "the button keeps its width";
    wait_idle(rig);
    EXPECT_EQ(dialog.status_label()->text(), "Could not reach GitHub. Check your internet connection and try again.");
    EXPECT_EQ(text_colour(dialog.status_label()), theme.failure);
    EXPECT_TRUE(dialog.check_now_button()->isEnabled());
    EXPECT_EQ(dialog.check_now_button()->text(), "Check now");
    EXPECT_EQ(dialog.version_label()->text(), q("WizzairHelper 1.0.0 · last successful check today at 10:00"))
        << "the last check that worked, named as such above the failure";
    EXPECT_EQ(dialog.size(), size) << "the window keeps its size";

    // A newer release: downloading (Check now waits), then ready, as the bar says.
    rig.clock->advance(std::chrono::minutes{5});
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.backend->hold_downloads();
    dialog.check_now_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->downloads_started() == 1; }));
    gui::settle();
    EXPECT_EQ(dialog.status_label()->text(), q("Downloading WizzairHelper 1.2.0…"));
    EXPECT_FALSE(dialog.check_now_button()->isEnabled()) << "no check during the download";
    EXPECT_EQ(dialog.check_now_button()->text(), "Check now");
    EXPECT_FALSE(rig.bar().isVisible()) << "install on close shows the bar once verified";
    rig.backend->release_downloads();
    ASSERT_TRUE(gui::wait_until([&] { return dialog.status().kind == UpdateStatusKind::ready; }));
    gui::settle();
    EXPECT_EQ(dialog.status_label()->text(), "WizzairHelper 1.2.0 is ready to install.");
    EXPECT_EQ(dialog.version_label()->text(), q("WizzairHelper 1.0.0 · last checked today at 10:10"));
    ASSERT_TRUE(rig.bar().isVisible());
    EXPECT_EQ(rig.bar().message_label()->text(),
              "WizzairHelper 1.2.0 is ready to install. It will be installed when you close WizzairHelper.");
    EXPECT_EQ(dialog.size(), size);

    // Install on close (the default): closing the app with the window open closes the
    // window, then installs without /RELAUNCH.
    rig.window().close();
    EXPECT_FALSE(rig.window().isVisible());
    ASSERT_TRUE(gui::wait_until([&] { return rig.window().settings_dialog() == nullptr; }));
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_FALSE(rig.launches.commands.front().command_line().ends_with(L" /RELAUNCH"));
}

TEST(SettingsWindow, Shows_an_update_put_off_earlier_from_the_start_without_a_request)
{
    UpdateRig rig{rig_options(options(WhenFound::download_then_ask))};
    rig.backend->schedule().record_success(kNewVersion);  // an earlier session today saw 1.2.0
    rig.backend->place_verified_installer(kNewVersion);
    rig.start();
    SettingsDialog& dialog = open_settings(rig.window());
    EXPECT_EQ(dialog.status().kind, UpdateStatusKind::ready);
    EXPECT_EQ(dialog.status_label()->text(), "WizzairHelper 1.2.0 is ready to install.");
    EXPECT_EQ(dialog.version_label()->text(), q("WizzairHelper 1.0.0 · last checked today at 10:00"));
    ASSERT_TRUE(rig.bar().isVisible());
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is ready to install.");

    // Only tell me: the same update is available, with Update now in the bar.
    dialog.when_found_button(WhenFound::notify_only)->click();
    gui::settle();
    EXPECT_EQ(dialog.status_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(rig.bar().action_button()->text(), "Update now");
    EXPECT_EQ(rig.backend->requests(), 0) << "changing the choice sends nothing either";
}

TEST(SettingsWindow, Download_then_ask_chosen_in_the_window_asks_and_closing_installs_nothing)
{
    UpdateRig rig;
    rig.start();
    SettingsDialog& dialog = open_settings(rig.window());
    dialog.when_found_button(WhenFound::download_then_ask)->click();
    rig.backend->script_checks({release_info(kNewVersion)});
    dialog.check_now_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return dialog.status().kind == UpdateStatusKind::ready; }));
    wait_idle(rig);
    EXPECT_EQ(rig.backend->downloaded(), std::vector<Version>{kNewVersion}) << "downloaded and verified";
    ASSERT_TRUE(rig.bar().isVisible());
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is ready to install.");
    EXPECT_EQ(rig.bar().action_button()->text(), "Restart and update");
    EXPECT_EQ(dialog.status_label()->text(), "WizzairHelper 1.2.0 is ready to install.");
    dialog.close_button()->click();
    rig.window().close();
    EXPECT_TRUE(rig.launches.commands.empty()) << "closing the app does not install it";
    EXPECT_EQ(saved(rig.window_rig).updates.when_found, WhenFound::download_then_ask);
}

TEST(SettingsWindow, Download_then_ask_chosen_in_the_window_installs_on_Restart_and_update)
{
    UpdateRig rig;
    rig.start();
    SettingsDialog& dialog = open_settings(rig.window());
    dialog.when_found_button(WhenFound::download_then_ask)->click();
    rig.backend->script_checks({release_info(kNewVersion)});
    dialog.check_now_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return rig.bar().isVisible(); }));
    dialog.close_button()->click();
    rig.bar().action_button()->click();
    EXPECT_FALSE(rig.window().isVisible());
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_TRUE(rig.launches.commands.front().command_line().ends_with(L" /RELAUNCH"));
}

TEST(SettingsWindow, Notify_only_chosen_in_the_window_tells_and_Update_now_restarts_into_the_installer)
{
    UpdateRig rig;
    rig.start();
    SettingsDialog& dialog = open_settings(rig.window());
    dialog.when_found_button(WhenFound::notify_only)->click();
    rig.backend->script_checks({release_info(kNewVersion)});
    dialog.check_now_button()->click();
    wait_idle(rig);
    EXPECT_TRUE(rig.backend->downloaded().empty()) << "nothing downloaded until the user asks";
    EXPECT_EQ(dialog.status().kind, UpdateStatusKind::available);
    EXPECT_EQ(dialog.status_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(text_colour(dialog.status_label()), style::theme().text);
    ASSERT_TRUE(rig.bar().isVisible());
    EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(rig.bar().action_button()->text(), "Update now");

    dialog.close_button()->click();
    rig.backend->hold_downloads();
    rig.bar().action_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return rig.backend->downloads_started() == 1; }));
    gui::settle();
    EXPECT_EQ(rig.bar().message_label()->text(), q("Downloading WizzairHelper 1.2.0…"));
    // The window opened during the download says the same.
    SettingsDialog& during = open_settings(rig.window());
    EXPECT_EQ(during.status_label()->text(), q("Downloading WizzairHelper 1.2.0…"));
    rig.backend->release_downloads();
    ASSERT_TRUE(gui::wait_until([&] { return !rig.window().isVisible(); })) << "verified: the app restarts";
    ASSERT_TRUE(gui::wait_until([&] { return rig.window().settings_dialog() == nullptr; })) << "with its window closed";
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_TRUE(rig.launches.commands.front().command_line().ends_with(L" /RELAUNCH"));
}

TEST(SettingsWindow, Switching_from_notify_only_to_install_on_close_downloads_at_once_and_closing_installs)
{
    UpdateRig rig{rig_options(options(WhenFound::notify_only))};
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.start();
    ASSERT_TRUE(rig.backend->downloaded().empty());
    SettingsDialog& dialog = open_settings(rig.window());
    EXPECT_EQ(dialog.status().kind, UpdateStatusKind::available);
    dialog.when_found_button(WhenFound::install_on_close)->click();
    ASSERT_TRUE(gui::wait_until([&] { return dialog.status().kind == UpdateStatusKind::ready; }));
    wait_idle(rig);
    EXPECT_EQ(rig.backend->downloaded(), std::vector<Version>{kNewVersion});
    EXPECT_EQ(rig.bar().message_label()->text(),
              "WizzairHelper 1.2.0 is ready to install. It will be installed when you close WizzairHelper.");
    rig.window().close();
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_FALSE(rig.launches.commands.front().command_line().ends_with(L" /RELAUNCH"));
}

TEST(SettingsWindow, With_checks_off_an_offered_update_keeps_Update_now_when_the_choice_changes)
{
    // Only tell me, checks off: Check now finds 1.2.0 and the bar offers Update now. Picking
    // a choice that downloads sends nothing on its own (the box is off), so the bar keeps
    // the offer and its action rather than losing both.
    UpdateRig rig{rig_options(options(WhenFound::notify_only, false))};
    rig.start();
    ASSERT_EQ(rig.backend->checks(), 0);
    SettingsDialog& dialog = open_settings(rig.window());
    rig.backend->script_checks({release_info(kNewVersion)});
    dialog.check_now_button()->click();
    wait_idle(rig);
    ASSERT_TRUE(rig.bar().isVisible());
    ASSERT_EQ(rig.bar().action_button()->text(), "Update now");

    for (const WhenFound choice : {WhenFound::install_on_close, WhenFound::download_then_ask}) {
        dialog.when_found_button(choice)->click();
        gui::settle();
        EXPECT_EQ(saved(rig.window_rig).updates, options(choice, false));
        EXPECT_TRUE(rig.backend->downloaded().empty()) << "checks are off: nothing is sent on its own";
        ASSERT_TRUE(rig.bar().isVisible()) << static_cast<int>(choice);
        EXPECT_EQ(rig.bar().message_label()->text(), "WizzairHelper 1.2.0 is available.");
        EXPECT_EQ(rig.bar().action_button()->text(), "Update now");
        EXPECT_EQ(dialog.status_label()->text(), "WizzairHelper 1.2.0 is available.");
    }

    dialog.close_button()->click();
    rig.bar().action_button()->click();
    ASSERT_TRUE(gui::wait_until([&] { return !rig.window().isVisible(); })) << "downloaded, verified, restarted";
    EXPECT_EQ(rig.backend->downloaded(), std::vector<Version>{kNewVersion});
    ASSERT_EQ(rig.launches.commands.size(), 1u);
    EXPECT_TRUE(rig.launches.commands.front().command_line().ends_with(L" /RELAUNCH"));
}

TEST(SettingsWindow, A_portable_copy_shows_the_install_choices_disabled_with_the_reason)
{
    UpdateRig rig{rig_options(options(WhenFound::install_on_close), InstallKind::portable)};
    rig.backend->script_checks({release_info(kNewVersion)});
    rig.start();
    SettingsDialog& dialog = open_settings(rig.window());
    for (const WhenFound choice : {WhenFound::install_on_close, WhenFound::download_then_ask, WhenFound::notify_only}) {
        EXPECT_FALSE(dialog.when_found_button(choice)->isEnabled()) << static_cast<int>(choice);
    }
    ASSERT_TRUE(dialog.note_label()->isVisible());
    EXPECT_EQ(dialog.note_label()->text(),
              "This portable copy cannot install updates itself; it links to the release page instead.");
    EXPECT_TRUE(dialog.check_automatically_box()->isEnabled()) << "the check options still apply";
    EXPECT_TRUE(dialog.frequency_box()->isEnabled());
    EXPECT_TRUE(dialog.check_now_button()->isEnabled());
    EXPECT_EQ(dialog.status_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(rig.bar().action_button()->text(), "Download") << "the release page";
    const std::vector<QWidget*> stops = gui::tab_stops(dialog);
    EXPECT_EQ(std::ranges::find(stops, dialog.when_found_button(WhenFound::notify_only)), stops.end());
    EXPECT_EQ(wizz::app::layout_problems(dialog), std::vector<std::string>{});

    dialog.frequency_box()->setCurrentIndex(1);
    EXPECT_EQ(saved(rig.window_rig).updates.check_frequency, CheckFrequency::weekly);
    EXPECT_TRUE(rig.backend->downloaded().empty());
}

TEST(SettingsWindow, Never_writes_settings_json_in_autotest_and_says_updates_are_off)
{
    QTemporaryDir out;
    WindowRigOptions setup;
    setup.scenario = "same_area";
    setup.autotest = wizz::ui::AutoTestOptions{};
    setup.autotest->output_dir = std::filesystem::path{out.path().toStdWString()};
    setup.autotest->to = "BUD";  // a search of 0 requests
    setup.autotest->keep_open = true;
    WindowRig rig{std::move(setup)};
    rig.window().autotest_runner()->set_screenshotter(
        [](const QWidget&, const std::filesystem::path&) -> std::expected<void, std::string> { return {}; });
    rig.start();
    ASSERT_EQ(rig.window().update_controller(), nullptr);

    SettingsDialog& dialog = open_settings(rig.window());
    EXPECT_EQ(dialog.status().kind, UpdateStatusKind::off);
    EXPECT_EQ(dialog.version_label()->text(), QStringLiteral("WizzairHelper " WIZZ_VERSION_STRING));
    EXPECT_EQ(dialog.status_label()->text(), "Updates are off in this run.");
    EXPECT_EQ(text_colour(dialog.status_label()), style::theme().text_muted);
    for (QWidget* control : std::initializer_list<QWidget*>{dialog.check_now_button(), dialog.check_automatically_box(),
                                                             dialog.frequency_box(),
                                                             dialog.when_found_button(WhenFound::install_on_close)}) {
        EXPECT_FALSE(control->isEnabled()) << control->metaObject()->className();
    }
    EXPECT_FALSE(dialog.note_label()->isVisible());
    EXPECT_TRUE(dialog.close_button()->isEnabled());
    EXPECT_EQ(dialog.focusWidget(), dialog.close_button()) << "the one control that works";
    EXPECT_EQ(dialog.options(), wizz::update::UpdateOptions{}) << "the settings' options, shown";

    // Even a change reported by the window writes nothing in a self-test run.
    Q_EMIT dialog.options_changed(options(WhenFound::notify_only, false));
    EXPECT_FALSE(std::filesystem::exists(settings_path(rig)));
    dialog.close_button()->click();
    rig.window().close();
    EXPECT_FALSE(std::filesystem::exists(settings_path(rig)));
}

TEST(SettingsWindow, Without_an_updater_it_says_updates_are_off_in_a_normal_run_too)
{
    // main never turns the updater on with --replay on the command line (D-45).
    WindowRig rig;
    rig.start();
    ASSERT_EQ(rig.window().update_controller(), nullptr);
    SettingsDialog& dialog = open_settings(rig.window());
    EXPECT_EQ(dialog.status(), wizz::ui::updates_off_view(*update::parse_version(WIZZ_VERSION_STRING)));
    EXPECT_FALSE(dialog.check_now_button()->isEnabled());
}

TEST(SettingsWindow, Tab_order_starting_focus_and_names_for_screen_readers)
{
    UpdateRig rig;
    rig.start();
    SettingsDialog& dialog = open_settings(rig.window());
    QRadioButton* install = dialog.when_found_button(WhenFound::install_on_close);
    QRadioButton* ask = dialog.when_found_button(WhenFound::download_then_ask);
    QRadioButton* notify = dialog.when_found_button(WhenFound::notify_only);
    EXPECT_EQ(dialog.focusWidget(), dialog.check_now_button()) << "focus starts on the first control";
    // Real Tab presses: an exclusive radio group is one stop, its selected choice (D-63).
    EXPECT_EQ(press_tab(dialog, 5), (std::vector<QWidget*>{dialog.check_automatically_box(), dialog.frequency_box(),
                                                            install, dialog.close_button(), dialog.check_now_button()}));
    EXPECT_EQ(press_tab(dialog, 2, true), (std::vector<QWidget*>{dialog.close_button(), install}));
    // The arrow keys move among the choices and select each one, which applies it at once.
    gui::send_key(install, QEvent::KeyPress, Qt::Key_Down);
    EXPECT_EQ(dialog.focusWidget(), ask);
    EXPECT_TRUE(ask->isChecked());
    EXPECT_EQ(rig.controller()->options().when_found, WhenFound::download_then_ask);
    EXPECT_EQ(saved(rig.window_rig).updates.when_found, WhenFound::download_then_ask);
    gui::send_key(ask, QEvent::KeyPress, Qt::Key_Down);
    EXPECT_EQ(dialog.focusWidget(), notify);
    EXPECT_EQ(rig.controller()->options().when_found, WhenFound::notify_only);
    EXPECT_EQ(press_tab(dialog, 1), (std::vector<QWidget*>{dialog.close_button()})) << "then on to Close";
    EXPECT_EQ(press_tab(dialog, 1, true), (std::vector<QWidget*>{notify})) << "back to the selected choice";

    EXPECT_EQ(dialog.updates_card()->accessibleName(), "Updates");
    EXPECT_EQ(dialog.updates_card()->title_label()->text(), "Updates");
    EXPECT_EQ(dialog.frequency_box()->accessibleName(), "How often to check");
    EXPECT_EQ(dialog.when_found_group()->accessibleName(), "When an update is found");
    EXPECT_EQ(dialog.when_found_caption()->text(), "When an update is found:");
    EXPECT_EQ(dialog.check_automatically_box()->text(), "Check for updates automatically");
    EXPECT_EQ(install->text(), "Download it and install when I close the app");
    EXPECT_EQ(ask->text(), "Download it, then ask me before installing");
    EXPECT_EQ(notify->text(), "Only tell me that an update is available");
    for (QWidget* control : std::initializer_list<QWidget*>{dialog.check_now_button(), dialog.check_automatically_box(),
                                                             dialog.frequency_box(), install, ask, notify,
                                                             dialog.close_button()}) {
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(control);
        ASSERT_NE(accessible, nullptr);
        EXPECT_FALSE(accessible->text(QAccessible::Name).isEmpty()) << control->metaObject()->className();
    }
    for (const QLabel* label : {dialog.version_label(), dialog.status_label(), dialog.when_found_caption(),
                                dialog.note_label()}) {
        EXPECT_EQ(label->textFormat(), Qt::PlainText) << "plain text (D-58)";
    }

    // Off: the drop-down leaves the Tab order.
    dialog.check_automatically_box()->click();
    const std::vector<QWidget*> off = gui::tab_stops(dialog);
    EXPECT_EQ(std::ranges::find(off, dialog.frequency_box()), off.end());
}

// ---------------------------------------------------------------------------
// Layout and look
// ---------------------------------------------------------------------------

/// Every status the window shows, as the update model words them.
[[nodiscard]] std::vector<std::pair<std::string, UpdateStatusView>> every_status()
{
    const std::string version = "WizzairHelper 2.1.0 · last checked on Mon 16 Nov 2026 at 09:05";
    const auto view = [&version](UpdateStatusKind kind, std::string status) {
        return UpdateStatusView{.kind = kind,
                                .version_line = version,
                                .status = std::move(status),
                                .checking = kind == UpdateStatusKind::checking,
                                .check_now_enabled = kind != UpdateStatusKind::checking &&
                                                     kind != UpdateStatusKind::downloading,
                                .options_enabled = true,
                                .when_found_enabled = true,
                                .when_found_note = {}};
    };
    UpdateStatusView portable = view(UpdateStatusKind::available, "WizzairHelper 22.10.10 is available.");
    portable.when_found_enabled = false;
    portable.when_found_note = std::string{strings::kPortableNote};
    UpdateStatusView never = view(UpdateStatusKind::not_checked, "");
    never.version_line = "WizzairHelper 2.1.0 · never checked";
    // After a failed check the version line names the last successful one (the longest
    // version line), or says there was none.
    UpdateStatusView failed =
        view(UpdateStatusKind::failed, "Could not reach GitHub. Check your internet connection and try again.");
    failed.version_line = "WizzairHelper 22.10.10 · last successful check on Wed 30 Sep 2026 at 23:59";
    UpdateStatusView failed_never = failed;
    failed_never.version_line = "WizzairHelper 22.10.10 · no successful check yet";
    return {
        {"not checked", never},
        {"latest", view(UpdateStatusKind::up_to_date, std::string{strings::kUpToDate})},
        {"checking", view(UpdateStatusKind::checking, std::string{strings::kChecking})},
        {"available", view(UpdateStatusKind::available, "WizzairHelper 22.10.10 is available.")},
        {"downloading", view(UpdateStatusKind::downloading, "Downloading WizzairHelper 22.10.10…")},
        {"ready", view(UpdateStatusKind::ready, "WizzairHelper 22.10.10 is ready to install.")},
        {"failed", failed},
        {"failed, never succeeded", failed_never},
        {"failed long", view(UpdateStatusKind::failed, "The download of WizzairHelper 22.10.10 did not match its "
                                                       "checksum and was deleted. Try again later.")},
        {"portable", portable},
        {"off", wizz::ui::updates_off_view(Version{2, 1, 0})},
    };
}

TEST(SettingsWindow, Lays_out_cleanly_in_every_state_and_fits_the_minimum_window)
{
    QWidget owner;
    owner.resize(gui::minimum_client_size());
    owner.show();
    const QSize minimum = gui::minimum_client_size();
    std::optional<int> usual_height;
    for (const auto& [name, status] : every_status()) {
        SettingsDialog dialog{update::UpdateOptions{}, status, &owner};
        dialog.show();
        gui::settle();
        EXPECT_EQ(wizz::app::layout_problems(dialog), std::vector<std::string>{}) << name;
        EXPECT_EQ(dialog.width(), SettingsDialog::kClientWidth) << name;
        EXPECT_LE(dialog.height(), minimum.height()) << name << ": fits the 980 x 600 window's screen";
        EXPECT_LE(dialog.width(), minimum.width()) << name;
        EXPECT_GE(dialog.status_label()->height(), dialog.status_label()->heightForWidth(dialog.status_label()->width()))
            << name << ": the status is not cut";
        EXPECT_TRUE(wizz::app::is_fully_visible(*dialog.close_button())) << name;
        EXPECT_EQ(dialog.note_label()->isVisible(), !status.when_found_note.empty()) << name;
        // The usual statuses (one or two lines) keep one height, so the window never jumps.
        if (status.when_found_note.empty() && name != "failed long") {
            if (!usual_height) {
                usual_height = dialog.height();
            }
            EXPECT_EQ(dialog.height(), *usual_height) << name;
        }
        dialog.close();
    }
}

TEST(SettingsWindow, Follows_the_look_in_light_dark_and_high_contrast)
{
    const wizz::test::look::LookGuard guard;
    qputenv("WIZZ_COLOR_SCHEME", "light");
    ASSERT_TRUE(style::apply_application_look());
    UpdateRig rig;
    rig.start();
    SettingsDialog& dialog = open_settings(rig.window());
    const auto expect_look = [&dialog](const char* look) {
        gui::settle();
        const style::Theme& theme = style::theme();
        EXPECT_EQ(dialog.palette().color(QPalette::Window), theme.window) << look;
        EXPECT_EQ(text_colour(dialog.status_label()), SettingsDialog::status_colour(dialog.status().kind)) << look;
        EXPECT_EQ(text_colour(dialog.when_found_caption()), theme.text_secondary) << look;
        const QImage card = dialog.updates_card()->grab().toImage();
        EXPECT_EQ(card.pixelColor(card.width() - 4, card.height() / 2), theme.surface) << look << ": the card";
        EXPECT_EQ(card.pixelColor(card.width() / 2, 0), theme.border) << look << ": its border";
        const QImage window = dialog.grab().toImage();
        EXPECT_EQ(window.pixelColor(2, window.height() / 2), theme.window) << look << ": behind the card";
        EXPECT_EQ(wizz::app::layout_problems(dialog), std::vector<std::string>{}) << look;
    };
    ASSERT_EQ(dialog.status().kind, UpdateStatusKind::up_to_date);
    expect_look("light");
    EXPECT_EQ(text_colour(dialog.status_label()), style::light_theme().success);

    qputenv("WIZZ_COLOR_SCHEME", "dark");
    ASSERT_TRUE(style::refresh_appearance());
    expect_look("dark");
    EXPECT_EQ(text_colour(dialog.status_label()), style::dark_theme().success) << "follows a live switch";

    style::set_high_contrast_probe([] { return true; });
    ASSERT_TRUE(style::refresh_appearance());
    ASSERT_EQ(style::appearance(), style::Appearance::high_contrast);
    ASSERT_TRUE(gui::wait_until([&] { return dialog.style() == QApplication::style(); }));
    expect_look("high contrast");
    EXPECT_EQ(text_colour(dialog.status_label()), QApplication::palette().color(QPalette::WindowText))
        << "no colour coding in High Contrast";
    EXPECT_EQ(wizz::app::layout_problems(rig.window()), std::vector<std::string>{}) << "the gear in High Contrast";
}

} // namespace
