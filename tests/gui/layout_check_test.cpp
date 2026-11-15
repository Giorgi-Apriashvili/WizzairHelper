// layout_check (01 §15, 05 §5): clipping, overlapping controls, cut texts and a hidden
// Cancel button are found automatically; the real main window has none.

#include "app_test_support.hpp"
#include "elided_label.hpp"
#include "layout_check.hpp"

#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWidget>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using wizz::app::describe_widget;
using wizz::app::is_fully_visible;
using wizz::app::layout_problems;
using wizz::test::app::WindowRig;

[[nodiscard]] bool has_problem(const std::vector<std::string>& problems, std::string_view text)
{
    return std::ranges::any_of(problems, [text](const std::string& problem) { return problem.contains(text); });
}

/// A shown window with a row of controls laid out by a QHBoxLayout.
struct Row {
    QWidget window;
    QLabel* label = new QLabel(QStringLiteral("Outbound between"));
    QPushButton* button = new QPushButton(QStringLiteral("Search outbound"));

    Row()
    {
        auto* layout = new QHBoxLayout(&window);
        layout->addWidget(label);
        layout->addWidget(button);
        layout->addStretch();
        window.resize(400, 60);
        window.show();
        QApplication::processEvents();
    }
};

TEST(LayoutCheck, A_proper_layout_has_no_problems)
{
    Row row;
    EXPECT_EQ(layout_problems(row.window), std::vector<std::string>{});
    EXPECT_TRUE(is_fully_visible(*row.button));
    EXPECT_EQ(describe_widget(*row.button), "QPushButton 'Search outbound'");
    QWidget hidden;
    EXPECT_TRUE(layout_problems(hidden).empty()) << "an invisible window is not checked";
    EXPECT_FALSE(is_fully_visible(hidden));
}

TEST(LayoutCheck, A_control_narrower_than_its_text_is_reported)
{
    Row row;
    row.button->setFixedWidth(20);
    QApplication::processEvents();
    const std::vector<std::string> problems = layout_problems(row.window);
    EXPECT_TRUE(has_problem(problems, "QPushButton 'Search outbound': smaller than its text")) << problems.size();
}

TEST(LayoutCheck, Labels_that_elide_or_ignore_their_size_are_exempt)
{
    QWidget window;
    auto* layout = new QHBoxLayout(&window);
    auto* elided = new wizz::app::ElidedLabel;
    elided->setText(QStringLiteral("Fetching fares:  a very long status line that does not fit at all"));
    auto* ignored = new QLabel(QStringLiteral("another label that is far too long for its cell"));
    ignored->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(elided);
    layout->addWidget(ignored);
    window.resize(60, 40);
    window.show();
    QApplication::processEvents();
    EXPECT_EQ(layout_problems(window), std::vector<std::string>{});
}

TEST(LayoutCheck, A_wrapped_label_lower_than_its_text_is_reported)
{
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* label = new QLabel(QStringLiteral("Outbound: from Budapest (BUD), Hungary → airports within 200 km: BUD 0 km, "
                                            "BTS 173 km, DEB 184 km. To London, United Kingdom — Wizz Air airports."));
    label->setWordWrap(true);
    layout->addWidget(label);
    window.resize(400, 200);
    window.show();
    QApplication::processEvents();
    EXPECT_EQ(layout_problems(window), std::vector<std::string>{});
    label->setFixedHeight(label->fontMetrics().height());  // one line for several
    QApplication::processEvents();
    EXPECT_TRUE(has_problem(layout_problems(window), "smaller than its text"));
}

TEST(LayoutCheck, Overlapping_and_clipped_controls_are_reported)
{
    Row row;
    row.button->move(row.label->pos());  // on top of the label (no relayout before the check)
    std::vector<std::string> problems = layout_problems(row.window);
    EXPECT_TRUE(has_problem(problems, "QLabel 'Outbound between': overlaps QPushButton 'Search outbound'"));

    row.window.layout()->invalidate();
    row.window.layout()->activate();  // back in place
    row.button->move(row.window.width() - row.button->width() / 2, row.button->y());
    problems = layout_problems(row.window);
    EXPECT_TRUE(has_problem(problems, "QPushButton 'Search outbound': clipped by its parent QWidget"));
    EXPECT_FALSE(is_fully_visible(*row.button)) << "half of Cancel outside the window is a hidden Cancel";
}

TEST(LayoutCheck, Tabs_that_do_not_fit_are_reported)
{
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* tabs = new QTabBar;
    for (const char* title : {"Outbound (123)", "Return (45)", "Round trips (678)", "Log"}) {
        tabs->addTab(QString::fromLatin1(title));
    }
    layout->addWidget(tabs);
    window.resize(tabs->sizeHint().width() + 40, 80);  // fonts differ between platforms
    window.show();
    QApplication::processEvents();
    EXPECT_FALSE(has_problem(layout_problems(window), "tabs do not fit"));
    tabs->setFixedWidth(60);
    QApplication::processEvents();
    EXPECT_TRUE(has_problem(layout_problems(window), "QTabBar: tabs do not fit"));
}

TEST(LayoutCheck, The_main_window_has_no_problems_and_Cancel_is_visible)
{
    WindowRig rig;
    rig.start();
    rig.run(rig.controller().search_outbound());
    // The "minimal" test platform's fallback font is about three times wider than Segoe UI
    // 9 pt, so give the window the room its texts need there (L(1320) x L(820) with real
    // fonts; the --autotest runs check the real sizes at 100/150/200 %).
    const QFontMetrics metrics{rig.window().font()};
    const double scale = std::max(1.0, metrics.horizontalAdvance(QStringLiteral("Search outbound")) / 100.0);
    rig.window().resize(static_cast<int>(1320 * scale), static_cast<int>(820 * std::max(1.0, scale / 1.5)));
    QApplication::processEvents();
    EXPECT_EQ(layout_problems(rig.window()), std::vector<std::string>{});
    EXPECT_TRUE(is_fully_visible(*rig.busy().cancel_button()));
    for (const wizz::app::MainWindow::Page page :
         {wizz::app::MainWindow::Page::inbound, wizz::app::MainWindow::Page::round_trips,
          wizz::app::MainWindow::Page::log}) {
        rig.window().select_page(page);
        QApplication::processEvents();
        EXPECT_EQ(layout_problems(rig.window()), std::vector<std::string>{}) << static_cast<int>(page);
    }
}

} // namespace
