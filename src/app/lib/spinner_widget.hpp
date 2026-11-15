#pragma once
// The busy spinner of 01 §10.8 (`SpinnerControl`): a custom-drawn, transparent
// ring with a magenta 100° arc while spinning and a small grey dot when idle.
// Geometry comes from wizz::ui::spinner_geometry (Qt-free, unit-tested); the
// widget only paints it. The BusyPanel drives the angle from its model on every
// Animate tick (24° per 40 ms, 600°/s).

#include <QWidget>

namespace wizz::app {

class SpinnerWidget final : public QWidget {
    Q_OBJECT

public:
    explicit SpinnerWidget(QWidget* parent = nullptr);

    [[nodiscard]] bool is_spinning() const noexcept { return spinning_; }
    /// Switches between the arc (spinning) and the idle dot; repaints on a change.
    void set_spinning(bool spinning);

    /// Start angle of the arc in degrees, GDI+ convention: clockwise from 3 o'clock.
    [[nodiscard]] int angle() const noexcept { return angle_; }
    /// Sets the angle (normalised to 0..359); repaints on a change.
    void set_angle(int degrees);

    /// `Step()`: when spinning, angle = (angle + 24) mod 360 and repaint.
    void step();

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool spinning_ = false;
    int angle_ = 0;
};

} // namespace wizz::app
