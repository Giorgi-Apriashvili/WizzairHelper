// SegmentedSwitch (DECISIONS D-75): the Settings window's "Auto | Dark | Light" — joined
// segments of one control height, as wide as the widest label plus the padding, one of them
// selected; a radio group for the keyboard and screen readers whose one Tab stop is the selected
// segment; painted from the theme in light, dark and High Contrast.

#include "segmented_switch.hpp"

#include "grid_test_support.hpp"
#include "look_test_support.hpp"
#include "modern_style.hpp"
#include "ui_style.hpp"

#include <QAccessible>
#include <QFontMetricsF>
#include <QImage>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtMath>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace {

namespace gui = wizz::test::gui;
namespace style = wizz::app::style;
using wizz::app::ModernStyle;
using wizz::app::SegmentedSwitch;

[[nodiscard]] QStringList theme_labels()
{
    return {QStringLiteral("Auto"), QStringLiteral("Dark"), QStringLiteral("Light")};
}

/// A shown window with a push button, which has the focus, and a switch under it.
struct Rig {
    std::unique_ptr<QWidget> window = std::make_unique<QWidget>();
    QPushButton* before = nullptr;
    SegmentedSwitch* control = nullptr;

    Rig()
    {
        auto* layout = new QVBoxLayout(window.get());
        before = new QPushButton(QStringLiteral("Before"), window.get());
        control = new SegmentedSwitch(theme_labels(), window.get());
        layout->addWidget(before);
        layout->addWidget(control);
        layout->addStretch(1);
        window->show();
        before->setFocus(Qt::OtherFocusReason);
        gui::settle();
    }

    /// The segments that take the focus with Tab.
    [[nodiscard]] std::vector<QWidget*> stops() const
    {
        std::vector<QWidget*> found = gui::tab_stops(*window);
        std::erase(found, before);
        return found;
    }
};

/// The colour at `point` of `segment` as the switch paints it.
[[nodiscard]] QColor pixel(SegmentedSwitch& control, int segment, QPoint point)
{
    const QImage image = control.grab().toImage();
    return image.pixelColor(control.segment(segment)->geometry().topLeft() + point);
}

TEST(SegmentedSwitch, D_75_Segments_are_joined_equally_wide_and_one_control_height_tall)
{
    Rig rig;
    SegmentedSwitch& control = *rig.control;
    ASSERT_EQ(control.count(), 3);
    const QFontMetricsF metrics(control.segment(0)->font());
    qreal widest = 0;
    for (const QString& label : theme_labels()) {
        widest = std::max(widest, metrics.horizontalAdvance(label));
    }
    const int width = qCeil(widest) + 2 * SegmentedSwitch::kPadding;
    const int height = ModernStyle::control_height(control.fontMetrics());
    int left = 0;
    for (int i = 0; i < control.count(); ++i) {
        EXPECT_EQ(control.segment(i)->geometry(), QRect(left, 0, width, height)) << i << ": no gap, no overlap";
        EXPECT_EQ(control.segment(i)->minimumSizeHint(), control.segment(i)->sizeHint());
        left += width;
    }
    EXPECT_EQ(control.size(), QSize(3 * width, height));
    EXPECT_EQ(control.sizeHint(), QSize(3 * width, height));
    EXPECT_EQ(height, 28) << "the height of a push button beside it (D-58)";
    EXPECT_EQ(control.segment(3), nullptr);
    EXPECT_EQ(control.segment(-1), nullptr);
}

TEST(SegmentedSwitch, D_75_One_segment_is_selected_and_a_change_is_reported_once)
{
    Rig rig;
    SegmentedSwitch& control = *rig.control;
    EXPECT_EQ(control.current(), 0) << "the first, at first";
    std::vector<int> reported;
    QObject::connect(&control, &SegmentedSwitch::current_changed, [&reported](int index) { reported.push_back(index); });
    control.segment(2)->click();
    EXPECT_EQ(control.current(), 2);
    EXPECT_FALSE(control.segment(0)->isChecked());
    control.set_current(1);
    control.set_current(1);
    control.set_current(7);  // out of range: ignored
    control.segment(1)->click();  // already selected: stays
    EXPECT_EQ(control.current(), 1);
    EXPECT_EQ(reported, (std::vector<int>{2, 1}));
    // Anywhere in a segment takes the click, not only its label.
    QRadioButton& light = *control.segment(2);
    gui::send_mouse(&light, QEvent::MouseButtonPress, QPoint(2, 2), Qt::LeftButton);
    gui::send_mouse(&light, QEvent::MouseButtonRelease, QPoint(2, 2), Qt::LeftButton);
    EXPECT_EQ(control.current(), 2);
}

TEST(SegmentedSwitch, D_75_The_one_Tab_stop_is_the_selected_segment_and_the_arrows_select)
{
    Rig rig;
    SegmentedSwitch& control = *rig.control;
    ASSERT_TRUE(rig.before->hasFocus());
    control.set_current(2);
    EXPECT_EQ(rig.stops(), std::vector<QWidget*>{control.segment(2)}) << "before any segment had the focus too";
    control.segment(2)->setFocus(Qt::TabFocusReason);
    gui::send_key(control.segment(2), QEvent::KeyPress, Qt::Key_Left);
    EXPECT_EQ(control.current(), 1);
    EXPECT_TRUE(control.segment(1)->hasFocus());
    EXPECT_EQ(rig.stops(), std::vector<QWidget*>{control.segment(1)});
    // Set from outside while a segment has the focus: that one stays the stop until it loses the
    // focus (Tab leaves the switch at once), then the selected one is.
    control.set_current(0);
    EXPECT_EQ(rig.stops(), std::vector<QWidget*>{control.segment(1)});
    rig.before->setFocus(Qt::OtherFocusReason);
    EXPECT_EQ(rig.stops(), std::vector<QWidget*>{control.segment(0)});
    // The focus on a segment that is not selected (a press dragged off it before the release): that
    // one is the stop while it has the focus, then the selected one again.
    control.segment(2)->setFocus(Qt::MouseFocusReason);
    ASSERT_TRUE(control.segment(2)->hasFocus());
    EXPECT_EQ(control.current(), 0);
    EXPECT_EQ(rig.stops(), std::vector<QWidget*>{control.segment(2)});
    rig.before->setFocus(Qt::OtherFocusReason);
    EXPECT_EQ(rig.stops(), std::vector<QWidget*>{control.segment(0)});
    // A click on a segment gives it the focus (the mouse focuses radio buttons) and selects it.
    for (int i = 0; i < control.count(); ++i) {
        EXPECT_TRUE(control.segment(i)->focusPolicy() & Qt::ClickFocus) << i;
    }
}

TEST(SegmentedSwitch, D_75_Screen_readers_hear_radio_buttons)
{
    Rig rig;
    SegmentedSwitch& control = *rig.control;
    control.set_current(1);
    for (int i = 0; i < control.count(); ++i) {
        QAccessibleInterface* accessible = QAccessible::queryAccessibleInterface(control.segment(i));
        ASSERT_NE(accessible, nullptr);
        EXPECT_EQ(accessible->role(), QAccessible::RadioButton) << i;
        EXPECT_EQ(accessible->text(QAccessible::Name), theme_labels().at(i));
        EXPECT_EQ(accessible->state().checked, i == 1) << i;
    }
}

TEST(SegmentedSwitch, D_75_Paints_the_selection_tint_an_accent_pill_dividers_and_the_frame)
{
    for (const char* scheme : {"light", "dark"}) {
        const wizz::test::look::LookGuard guard;
        qputenv("WIZZ_COLOR_SCHEME", scheme);
        ASSERT_TRUE(style::apply_application_look());
        const style::Theme& t = style::theme();
        Rig rig;
        SegmentedSwitch& control = *rig.control;
        control.set_current(1);  // Dark: in the middle
        gui::settle();
        const int w = control.segment(0)->width();
        const int h = control.segment(0)->height();
        EXPECT_EQ(control.fill_colour(0), t.button) << scheme;
        EXPECT_EQ(control.fill_colour(1), t.selection) << scheme;
        EXPECT_EQ(control.label_colour(0), t.text_secondary) << scheme;
        EXPECT_EQ(control.label_colour(1), t.selection_text) << scheme;
        EXPECT_EQ(pixel(control, 0, {4, 4}), t.button) << scheme << ": an unselected segment";
        EXPECT_EQ(pixel(control, 1, {4, 4}), t.selection) << scheme << ": the selected one";
        EXPECT_EQ(pixel(control, 1, {w / 2, h - 3}), t.accent_line) << scheme << ": its pill";
        EXPECT_EQ(pixel(control, 0, {w / 2, h - 3}), t.button) << scheme << ": no pill";
        EXPECT_EQ(pixel(control, 1, {w / 2, 0}), t.input_border) << scheme << ": the frame, top";
        EXPECT_EQ(pixel(control, 1, {w / 2, h - 1}), t.input_border) << scheme << ": the frame, bottom";
        EXPECT_EQ(pixel(control, 0, {0, h / 2}), t.input_border) << scheme << ": the frame, left";
        EXPECT_EQ(pixel(control, 2, {w - 1, h / 2}), t.input_border) << scheme << ": the frame, right";
        // No divider beside the selection; one between two unselected segments.
        EXPECT_EQ(pixel(control, 0, {w - 1, h / 2}), t.button) << scheme;
        control.set_current(2);
        gui::settle();
        EXPECT_EQ(pixel(control, 0, {w - 1, h / 2}), t.border) << scheme << ": Auto | Dark";
        EXPECT_EQ(pixel(control, 0, {w - 1, 2}), t.button) << scheme << ": the divider is inset";
        EXPECT_EQ(pixel(control, 1, {w - 1, h / 2}), t.button) << scheme << ": none beside Light";

        // Hover and pressed fills on an unselected segment, like a secondary button; the label darker.
        QRadioButton& auto_segment = *control.segment(0);
        auto_segment.setAttribute(Qt::WA_UnderMouse);
        EXPECT_EQ(control.fill_colour(0), t.button_hover) << scheme;
        EXPECT_EQ(control.label_colour(0), t.text) << scheme;
        auto_segment.setDown(true);
        EXPECT_EQ(control.fill_colour(0), t.button_pressed) << scheme;
        auto_segment.setDown(false);
        auto_segment.setAttribute(Qt::WA_UnderMouse, false);
        control.setEnabled(false);
        EXPECT_EQ(control.fill_colour(2), t.input_disabled) << scheme;
        EXPECT_EQ(control.label_colour(2), t.text_disabled) << scheme;
    }
}

TEST(SegmentedSwitch, D_75_High_contrast_marks_the_selection_with_the_system_highlight)
{
    const wizz::test::look::LookGuard guard;
    style::set_high_contrast_probe([] { return true; });
    ASSERT_TRUE(style::apply_application_look());
    ASSERT_EQ(style::appearance(), style::Appearance::high_contrast);
    Rig rig;
    SegmentedSwitch& control = *rig.control;
    control.set_current(1);
    gui::settle();
    const QPalette system = QApplication::palette();
    const int w = control.segment(1)->width();
    const int h = control.segment(1)->height();
    EXPECT_EQ(control.fill_colour(1), system.color(QPalette::Highlight));
    EXPECT_EQ(control.label_colour(1), system.color(QPalette::HighlightedText));
    EXPECT_EQ(control.fill_colour(0), system.color(QPalette::Button));
    EXPECT_EQ(control.label_colour(0), system.color(QPalette::WindowText));
    EXPECT_EQ(pixel(control, 1, {w / 2, h - 3}), system.color(QPalette::Highlight)) << "no pill, the fill says it";
    EXPECT_EQ(pixel(control, 1, {w / 2, 0}), system.color(QPalette::WindowText)) << "the frame";

    // The keyboard focus: the plain style's dotted frame in the inverse of the Highlight fill under
    // it, not of the Button colour (all but invisible on some contrast themes' Highlight).
    rig.window->setAttribute(Qt::WA_KeyboardFocusChange);
    control.segment(1)->setFocus(Qt::TabFocusReason);
    rig.window->setAttribute(Qt::WA_KeyboardFocusChange);
    control.segment(1)->update();
    gui::settle();
    const auto inverse = [](const QColor& colour) {
        return QColor(colour.red() ^ 0xFF, colour.green() ^ 0xFF, colour.blue() ^ 0xFF);
    };
    const QColor dots = inverse(system.color(QPalette::Highlight));
    const QColor wrong = inverse(system.color(QPalette::Button));
    const QImage image = control.grab().toImage();
    const QRect segment = control.segment(1)->geometry();
    int on_fill = 0;
    int of_button = 0;
    for (int x = segment.left() + 4; x < segment.right() - 4; ++x) {
        const QColor at = image.pixelColor(x, segment.top() + 2);
        on_fill += at == dots ? 1 : 0;
        of_button += at == wrong && wrong != dots ? 1 : 0;
    }
    EXPECT_GT(on_fill, 0) << "the dots: " << dots.name().toStdString();
    EXPECT_EQ(of_button, 0);
}

} // namespace
