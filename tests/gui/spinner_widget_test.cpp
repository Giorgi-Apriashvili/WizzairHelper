// SpinnerWidget (01 §10.8): rendered into transparent images at several sizes and
// angles; pixels on the ring, on the arc and at the centre are checked against the
// geometry of wizz::ui::spinner_geometry (GDI+ conventions: angles clockwise from
// 3 o'clock, pixel centres on integer coordinates).

#include "diagnostics_test_support.hpp"
#include "look_test_support.hpp"
#include "spinner_widget.hpp"
#include "ui_style.hpp"

#include "wizz/ui/busy_panel_model.hpp"

#include <QImage>
#include <QPainter>

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <optional>

namespace {

namespace style = wizz::app::style;
using wizz::app::SpinnerWidget;
using wizz::test::save_snapshot;

struct Sample {
    int alpha = 0;
    int red = 0;
    int green = 0;
    int blue = 0;
};

class SpinnerRender {
public:
    SpinnerRender(int width, int height, bool spinning, int angle) : image_(width, height, QImage::Format_ARGB32)
    {
        SpinnerWidget spinner;
        spinner.resize(width, height);
        spinner.set_spinning(spinning);
        spinner.set_angle(angle);
        image_.fill(Qt::transparent);
        spinner.render(&image_, QPoint(), QRegion(), QWidget::RenderFlags());  // no background: transparent
        geometry_ = wizz::ui::spinner_geometry(width, height);
    }

    [[nodiscard]] const QImage& image() const { return image_; }
    [[nodiscard]] const std::optional<wizz::ui::SpinnerGeometry>& geometry() const { return geometry_; }

    /// The pixel whose centre is nearest to the GDI+ point at `degrees` (clockwise from
    /// 3 o'clock) on a circle of `radius` around the ring centre.
    [[nodiscard]] Sample at_angle(double degrees, double radius) const
    {
        const double centre_x = geometry_->left + geometry_->size / 2.0;
        const double centre_y = geometry_->top + geometry_->size / 2.0;
        const double radians = degrees * std::numbers::pi / 180.0;
        // GDI+ point (x, y) is the centre of pixel (round(x), round(y)).
        return at(static_cast<int>(std::lround(centre_x + radius * std::cos(radians))),
                  static_cast<int>(std::lround(centre_y + radius * std::sin(radians))));
    }

    [[nodiscard]] Sample at(int x, int y) const
    {
        const QColor colour = image_.pixelColor(x, y);
        return {colour.alpha(), colour.red(), colour.green(), colour.blue()};
    }

    [[nodiscard]] double radius() const { return geometry_->size / 2.0; }

    [[nodiscard]] bool fully_transparent() const
    {
        for (int y = 0; y < image_.height(); ++y) {
            for (int x = 0; x < image_.width(); ++x) {
                if (image_.pixelColor(x, y).alpha() != 0) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool has_magenta() const
    {
        for (int y = 0; y < image_.height(); ++y) {
            for (int x = 0; x < image_.width(); ++x) {
                const QColor c = image_.pixelColor(x, y);
                if (c.alpha() > 0 && c.red() > 100 && c.green() < 60) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    QImage image_;
    std::optional<wizz::ui::SpinnerGeometry> geometry_;
};

/// The arc: the accent (#C5006E) in light, the lighter accent line in dark (D-53).
void expect_magenta(const Sample& s)
{
    const style::Theme& theme = style::theme();
    const QColor arc = theme.dark ? theme.accent_line : theme.accent;
    EXPECT_GE(s.alpha, 200);
    EXPECT_NEAR(s.red, arc.red(), 30);
    EXPECT_NEAR(s.green, arc.green(), 30);
    EXPECT_NEAR(s.blue, arc.blue(), 30);
}

/// Translucent text colour: 01 §10.8's ARGB(40, 0, 0, 0) ring became the theme's text at
/// alpha 40 (50 in dark), so it shows on either surface; the idle dot at alpha 120.
void expect_text_at_alpha(const Sample& s, int alpha)
{
    const QColor text = style::theme().text;
    EXPECT_NEAR(s.alpha, alpha, 12);
    // A translucent pixel keeps its colour only roughly (premultiplied rounding).
    EXPECT_NEAR(s.red, text.red(), 10);
    EXPECT_NEAR(s.green, text.green(), 10);
    EXPECT_NEAR(s.blue, text.blue(), 10);
}

void expect_track(const Sample& s)
{
    expect_text_at_alpha(s, style::theme().dark ? 50 : 40);
}

TEST(SpinnerWidget, Starts_idle_at_angle_0)
{
    const SpinnerWidget spinner;
    EXPECT_FALSE(spinner.is_spinning());
    EXPECT_EQ(spinner.angle(), 0);
    EXPECT_FALSE(spinner.autoFillBackground());
}

TEST(SpinnerWidget, Step_advances_24_degrees_only_while_spinning)
{
    SpinnerWidget spinner;
    spinner.step();
    EXPECT_EQ(spinner.angle(), 0);

    spinner.set_spinning(true);
    spinner.step();
    EXPECT_EQ(spinner.angle(), 24);
    spinner.set_angle(350);
    spinner.step();
    EXPECT_EQ(spinner.angle(), 14);  // (350 + 24) mod 360
    for (int i = 0; i < 15; ++i) {
        spinner.step();
    }
    EXPECT_EQ(spinner.angle(), 14);  // 15 × 24 = 360

    spinner.set_angle(370);
    EXPECT_EQ(spinner.angle(), 10);
    spinner.set_angle(-30);
    EXPECT_EQ(spinner.angle(), 330);
}

TEST(SpinnerWidget, Draws_nothing_below_8_px)
{
    const SpinnerRender tiny(15, 40, true, 0);  // min(W, H) − 8 = 7
    EXPECT_FALSE(tiny.geometry().has_value());
    EXPECT_TRUE(tiny.fully_transparent());

    const SpinnerRender smallest(16, 16, true, 0);  // size 8: drawn
    ASSERT_TRUE(smallest.geometry().has_value());
    EXPECT_FALSE(smallest.fully_transparent());
}

TEST(SpinnerWidget, Busy_panel_size_draws_track_and_magenta_arc_clockwise_from_3_o_clock)
{
    // The panel's spinner cell: 30 px wide (36 − 6), two rows high.
    const SpinnerRender render(30, 43, true, 0);
    ASSERT_TRUE(render.geometry().has_value());
    EXPECT_EQ(render.geometry()->size, 22);
    EXPECT_FLOAT_EQ(render.geometry()->thickness, 22.0F / 7.0F);
    save_snapshot(render.image(), QStringLiteral("spinner-30x43-0"));

    const double r = render.radius();
    expect_magenta(render.at_angle(50, r));   // middle of the 0°..100° sweep (below the right side)
    expect_magenta(render.at_angle(10, r));
    expect_magenta(render.at_angle(90, r));   // 6 o'clock
    expect_track(render.at_angle(230, r));    // opposite side: only the ring
    expect_track(render.at_angle(300, r));    // counter-clockwise of the start (above 3 o'clock)
    EXPECT_EQ(render.at_angle(0, 0).alpha, 0);       // centre stays transparent while spinning
    EXPECT_EQ(render.at(0, 0).alpha, 0);              // outside the ring
}

TEST(SpinnerWidget, Arc_starts_at_the_angle)
{
    const SpinnerRender render(36, 36, true, 180);  // 9 o'clock, sweeping up over 12 o'clock
    save_snapshot(render.image(), QStringLiteral("spinner-36x36-180"));
    const double r = render.radius();
    expect_magenta(render.at_angle(230, r));
    expect_magenta(render.at_angle(270, r));  // 12 o'clock
    expect_track(render.at_angle(50, r));
    expect_track(render.at_angle(130, r));
}

TEST(SpinnerWidget, Thickness_scales_with_size)
{
    const SpinnerRender render(100, 100, true, 0);  // size 92, thickness 13.14
    ASSERT_TRUE(render.geometry().has_value());
    EXPECT_NEAR(render.geometry()->thickness, 92.0 / 7.0, 1e-5);
    save_snapshot(render.image(), QStringLiteral("spinner-100x100-0"));

    const double r = render.radius();
    expect_magenta(render.at_angle(50, r - 5));  // inside the 13 px stroke
    expect_magenta(render.at_angle(50, r + 5));
    EXPECT_EQ(render.at_angle(50, r - 9).alpha, 0);   // beyond the stroke
    EXPECT_EQ(render.at_angle(230, r - 9).alpha, 0);
    expect_track(render.at_angle(230, r + 5));
}

TEST(SpinnerWidget, Idle_draws_the_ring_and_a_grey_centre_dot)
{
    const SpinnerRender render(30, 43, false, 120);
    ASSERT_TRUE(render.geometry().has_value());
    EXPECT_EQ(render.geometry()->dot_diameter, 5);  // (int)3.14 + 2
    save_snapshot(render.image(), QStringLiteral("spinner-30x43-idle"));

    EXPECT_FALSE(render.has_magenta());
    const auto& g = *render.geometry();
    const Sample dot = render.at(g.dot_left + g.dot_diameter / 2, g.dot_top + g.dot_diameter / 2);
    expect_text_at_alpha(dot, 120);  // 01: ARGB(120, 0, 0, 0)
    expect_track(render.at_angle(50, render.radius()));
    expect_track(render.at_angle(230, render.radius()));
}

TEST(SpinnerWidget, Paints_inside_its_contents_rect)
{
    SpinnerWidget spinner;
    spinner.setContentsMargins(0, 0, 6, 0);  // the busy panel's "Margin right 6"
    spinner.resize(36, 43);
    spinner.set_spinning(true);
    QImage image(36, 43, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    spinner.render(&image, QPoint(), QRegion(), QWidget::RenderFlags());

    for (int y = 0; y < image.height(); ++y) {
        for (int x = 30; x < 36; ++x) {
            EXPECT_EQ(image.pixelColor(x, y).alpha(), 0) << x << "," << y;
        }
    }
    EXPECT_GT(image.pixelColor(26, 21).alpha(), 0);  // the ring's right side (3 o'clock), at 30 − 4
}

TEST(SpinnerWidget, Dark_draws_the_lighter_accent_and_a_light_ring)
{
    // On the dark surface the arc is the accent line (#F05AA8, 3:1 on dark surfaces) and
    // the ring and dot are the light text colour, so the spinner stays visible.
    wizz::test::look::LookGuard guard;
    qputenv("WIZZ_COLOR_SCHEME", "dark");
    ASSERT_TRUE(style::apply_application_look());
    ASSERT_TRUE(style::theme().dark);
    const SpinnerRender render(30, 43, true, 0);
    ASSERT_TRUE(render.geometry().has_value());
    const double r = render.radius();
    const Sample arc = render.at_angle(50, r);
    EXPECT_GE(arc.alpha, 200);
    EXPECT_NEAR(arc.red, 0xF0, 30);
    EXPECT_NEAR(arc.green, 0x5A, 30);
    EXPECT_NEAR(arc.blue, 0xA8, 30);
    expect_track(render.at_angle(230, r));
    EXPECT_GT(render.at_angle(230, r).red, 180) << "a light ring";

    const SpinnerRender idle(30, 43, false, 0);
    const auto& g = *idle.geometry();
    expect_text_at_alpha(idle.at(g.dot_left + g.dot_diameter / 2, g.dot_top + g.dot_diameter / 2), 120);
}

} // namespace
