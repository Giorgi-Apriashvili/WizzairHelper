// The update bar (D-46): hidden until an update is offered, the exact texts, the
// magenta action and the small "Later", never taking the focus, its place between
// the input panel and the result tabs, the Tab order, and a layout without clipped
// or cut controls at the minimum window size (01 §15). With WIZZ_GUI_SNAPSHOT_DIR
// set it saves the window with the bar for a visual review (run with
// QT_QPA_PLATFORM=windows and QT_SCALE_FACTOR=1.5 / 2 for the DPI checks).

#include "update_test_support.hpp"

#include "diagnostics_test_support.hpp"
#include "input_panel.hpp"
#include "layout_check.hpp"
#include "search_button.hpp"
#include "ui_style.hpp"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using wizz::app::UpdateBar;
using wizz::test::app::release_info;
using wizz::test::app::UpdateRig;
using wizz::test::app::UpdateRigOptions;
using wizz::ui::UpdateBarView;
namespace gui = wizz::test::gui;

const UpdateBarView kReady{
    .visible = true,
    .text = "WizzairHelper 1.2.0 is ready to install. It will be installed when you close WizzairHelper.",
    .action = "Restart and update",
    .action_enabled = true};
const UpdateBarView kAvailable{
    .visible = true, .text = "WizzairHelper 1.2.0 is available.", .action = "Download", .action_enabled = true};

/// A shown window holding a focused line edit and the bar.
struct BarWindow {
    QWidget window;
    QPushButton* other = new QPushButton(QStringLiteral("Search outbound"));
    UpdateBar* bar = new UpdateBar;

    BarWindow()
    {
        auto* layout = new QVBoxLayout(&window);
        layout->addWidget(other);
        layout->addWidget(bar);
        window.resize(900, 120);
        window.show();
        other->setFocus();
        gui::settle();
    }
};

TEST(UpdateBar, Is_hidden_until_an_update_is_offered)
{
    BarWindow w;
    EXPECT_FALSE(w.bar->isVisible());
    EXPECT_EQ(w.bar->view(), UpdateBarView{});
    w.bar->show_view(kReady);
    gui::settle();
    EXPECT_TRUE(w.bar->isVisible());
    w.bar->show_view(UpdateBarView{});
    gui::settle();
    EXPECT_FALSE(w.bar->isVisible());
}

TEST(UpdateBar, Shows_the_texts_and_buttons_of_its_view)
{
    BarWindow w;
    w.bar->show_view(kReady);
    gui::settle();
    EXPECT_EQ(w.bar->message_label()->text(),
              "WizzairHelper 1.2.0 is ready to install. It will be installed when you close WizzairHelper.");
    EXPECT_EQ(w.bar->message_label()->textFormat(), Qt::PlainText);
    EXPECT_TRUE(w.bar->message_label()->wordWrap()) << "wraps rather than clips at high DPI";
    EXPECT_EQ(w.bar->action_button()->text(), "Restart and update");
    EXPECT_TRUE(w.bar->action_button()->isEnabled());
    EXPECT_EQ(w.bar->action_button()->background_color(), wizz::app::style::wizz_magenta());
    EXPECT_EQ(w.bar->later_button()->text(), "Later");
    EXPECT_TRUE(w.bar->later_button()->isFlat()) << "a small, quiet Later";
    EXPECT_FALSE(w.bar->action_button()->autoDefault());
    EXPECT_FALSE(w.bar->later_button()->autoDefault());
    EXPECT_EQ(w.bar->message_label()->font().family(), wizz::app::style::form_font().family());

    UpdateBarView disabled = kReady;
    disabled.action_enabled = false;
    w.bar->show_view(disabled);
    EXPECT_FALSE(w.bar->action_button()->isEnabled());
    EXPECT_TRUE(w.bar->later_button()->isEnabled());

    w.bar->show_view(kAvailable);
    EXPECT_EQ(w.bar->message_label()->text(), "WizzairHelper 1.2.0 is available.");
    EXPECT_EQ(w.bar->action_button()->text(), "Download");
    EXPECT_TRUE(w.bar->action_button()->isEnabled());
}

TEST(UpdateBar, Never_takes_the_focus_and_reports_its_clicks)
{
    BarWindow w;
    ASSERT_EQ(QApplication::focusWidget(), w.other);
    w.bar->show_view(kReady);
    gui::settle();
    EXPECT_EQ(QApplication::focusWidget(), w.other);

    int action = 0;
    int later = 0;
    QObject::connect(w.bar, &UpdateBar::action_clicked, [&action] { ++action; });
    QObject::connect(w.bar, &UpdateBar::later_clicked, [&later] { ++later; });
    w.bar->action_button()->click();
    EXPECT_EQ(action, 1);
    EXPECT_EQ(later, 0);
    w.bar->later_button()->click();
    EXPECT_EQ(later, 1);
}

TEST(UpdateBar, Paints_a_light_magenta_tint)
{
    const QColor tint = UpdateBar::background_colour();
    EXPECT_GT(tint.lightness(), 220) << "a light background keeps the text readable";
    EXPECT_GT(tint.red(), tint.green()) << "a tint of the Wizz magenta";
    EXPECT_GT(tint.blue(), tint.green());
}

TEST(UpdateBar, Follows_the_input_panel_in_the_tab_order_while_visible)
{
    UpdateRig rig;
    rig.backend->script_checks({release_info(wizz::update::Version{1, 2, 0})});
    rig.start();
    ASSERT_TRUE(gui::wait_until([&] { return rig.bar().isVisible(); }, 10s));
    const std::vector<QWidget*> stops = gui::tab_stops(rig.window());
    const auto at = [&](QWidget* widget) { return std::ranges::find(stops, widget) - stops.begin(); };
    const std::vector<QWidget*> inputs = rig.window_rig.input().tab_order();
    ASSERT_FALSE(inputs.empty());
    EXPECT_EQ(at(rig.bar().action_button()), at(inputs.back()) + 1);
    EXPECT_EQ(at(rig.bar().later_button()), at(rig.bar().action_button()) + 1);
    EXPECT_EQ(at(rig.window().tabs()->tabBar()), at(rig.bar().later_button()) + 1);

    rig.bar().later_button()->click();
    const std::vector<QWidget*> hidden = gui::tab_stops(rig.window());
    EXPECT_EQ(std::ranges::find(hidden, rig.bar().action_button()), hidden.end()) << "a hidden bar is skipped";
}

/// Checks the bar, and the whole window when its fonts are real: the "minimal" test
/// platform's fallback font is about three times wider than Segoe UI 9 pt, so there the
/// input panel's own hints need more room than L(980) (layout_check_test.cpp).
void expect_clean_layout(UpdateRig& rig, const char* where)
{
    gui::settle();
    EXPECT_EQ(wizz::app::layout_problems(rig.bar()), std::vector<std::string>{}) << where;
    EXPECT_TRUE(wizz::app::is_fully_visible(*rig.bar().action_button())) << where;
    EXPECT_TRUE(wizz::app::is_fully_visible(*rig.bar().later_button())) << where;
    EXPECT_GE(rig.bar().message_label()->height(),
              rig.bar().message_label()->heightForWidth(rig.bar().message_label()->width()))
        << where << ": the wrapped message is not cut";
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        EXPECT_EQ(wizz::app::layout_problems(rig.window()), std::vector<std::string>{}) << where;
    }
}

TEST(UpdateBar, Lays_out_without_clipping_in_the_main_window)
{
    // A visual review in the app's own look: WIZZ_GUI_SNAPSHOT_STYLE=windowsvista.
    if (const QString style = qEnvironmentVariable("WIZZ_GUI_SNAPSHOT_STYLE"); !style.isEmpty()) {
        QApplication::setStyle(style);
    }
    for (const wizz::update::InstallKind kind : {wizz::update::InstallKind::installed, wizz::update::InstallKind::portable}) {
        UpdateRigOptions options;
        options.kind = kind;
        UpdateRig rig{std::move(options)};
        rig.backend->script_checks({release_info(wizz::update::Version{1, 2, 0})});
        rig.start();
        ASSERT_TRUE(gui::wait_until([&] { return rig.bar().isVisible(); }, 10s));
        const QString name = kind == wizz::update::InstallKind::installed ? QStringLiteral("update_bar_ready")
                                                                          : QStringLiteral("update_bar_available");

        expect_clean_layout(rig, "default size");
        wizz::test::save_snapshot(rig.window(), name);

        // The narrowest window (01 §4.1 minimum): the message wraps, nothing is cut.
        rig.window().resize(rig.window().minimumSize());
        expect_clean_layout(rig, "minimum size");
        wizz::test::save_snapshot(rig.window(), name + QStringLiteral("_minimum"));

        // The whole window with room for the minimal platform's wide font.
        const QFontMetrics metrics{rig.window().font()};
        const double scale = std::max(1.0, metrics.horizontalAdvance(QStringLiteral("Search outbound")) / 100.0);
        rig.window().resize(static_cast<int>(1320 * scale), static_cast<int>(820 * std::max(1.0, scale / 1.5)));
        gui::settle();
        EXPECT_EQ(wizz::app::layout_problems(rig.window()), std::vector<std::string>{});
    }
}

TEST(UpdateBar, Wraps_its_message_when_the_width_is_short)
{
    BarWindow w;
    w.bar->show_view(kReady);
    gui::settle();
    const int line = w.bar->message_label()->fontMetrics().height();
    const QSize buttons = w.bar->action_button()->sizeHint() + QSize{w.bar->later_button()->sizeHint().width(), 0};
    // Room for the buttons and a few words of the message.
    w.window.resize(buttons.width() + 12 * w.bar->message_label()->fontMetrics().averageCharWidth() + 60, 400);
    gui::settle();
    EXPECT_EQ(wizz::app::layout_problems(w.window), std::vector<std::string>{});
    EXPECT_GE(w.bar->message_label()->height(), 3 * line) << "the message takes the lines it needs";
    EXPECT_EQ(w.bar->action_button()->size(), w.bar->action_button()->sizeHint()) << "buttons keep their size";
    EXPECT_EQ(w.bar->later_button()->size(), w.bar->later_button()->sizeHint());
    EXPECT_TRUE(wizz::app::is_fully_visible(*w.bar->action_button()));
    EXPECT_TRUE(wizz::app::is_fully_visible(*w.bar->later_button()));
    wizz::test::save_snapshot(w.window, QStringLiteral("update_bar_wrapped"));
}

} // namespace
