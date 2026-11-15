#include "spinner_widget.hpp"

#include "wizz/ui/busy_panel_model.hpp"

#include <QPainter>
#include <QPen>

namespace wizz::app {

namespace {

// 01 §10.8 colours.
constexpr QColor kTrackColour{0, 0, 0, 40};     // ARGB(40, 0, 0, 0)
constexpr QColor kArcColour{197, 0, 110};       // RGB(197, 0, 110), the Wizz magenta
constexpr QColor kIdleDotColour{0, 0, 0, 120};  // ARGB(120, 0, 0, 0)

/// Qt angles are counter-clockwise in 1/16 degree; GDI+ angles run clockwise.
constexpr int kQtDegree = 16;

} // namespace

SpinnerWidget::SpinnerWidget(QWidget* parent) : QWidget(parent)
{
    // Transparent: the parent's background shows through (no autoFillBackground).
    setAttribute(Qt::WA_NoSystemBackground);
}

void SpinnerWidget::set_spinning(bool spinning)
{
    if (spinning_ == spinning) {
        return;
    }
    spinning_ = spinning;
    update();
}

void SpinnerWidget::set_angle(int degrees)
{
    const int normalised = ((degrees % 360) + 360) % 360;
    if (angle_ == normalised) {
        return;
    }
    angle_ = normalised;
    if (spinning_) {
        update();
    }
}

void SpinnerWidget::step()
{
    if (!spinning_) {
        return;
    }
    angle_ = ui::next_spinner_angle(angle_);
    update();
}

QSize SpinnerWidget::sizeHint() const
{
    const QMargins margins = contentsMargins();
    return {30 + margins.left() + margins.right(), 30 + margins.top() + margins.bottom()};
}

void SpinnerWidget::paintEvent(QPaintEvent* /*event*/)
{
    const QRect area = contentsRect();
    const std::optional<ui::SpinnerGeometry> geometry = ui::spinner_geometry(area.width(), area.height());
    if (!geometry) {
        return;  // size < 8: draw nothing
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    // GDI+ (PixelOffsetMode.None) puts pixel centres on integer coordinates, Qt on
    // half-integers: shift by half a pixel so the shapes land where GDI+ draws them.
    painter.translate(QPointF(area.topLeft()) + QPointF(0.5, 0.5));

    const QRectF circle(geometry->left, geometry->top, geometry->size, geometry->size);
    const qreal thickness = geometry->thickness;

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(kTrackColour, thickness, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
    painter.drawEllipse(circle);

    if (spinning_) {
        painter.setPen(QPen(kArcColour, thickness, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        // GDI+ DrawArc(start = angle, sweep = 100) runs clockwise on screen: negate for Qt.
        painter.drawArc(circle, -angle_ * kQtDegree, -geometry->sweep_degrees * kQtDegree);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(kIdleDotColour);
        painter.drawEllipse(QRectF(geometry->dot_left, geometry->dot_top, geometry->dot_diameter,
                                   geometry->dot_diameter));
    }
}

} // namespace wizz::app
