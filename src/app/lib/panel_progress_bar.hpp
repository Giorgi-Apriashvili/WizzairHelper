#pragma once
// The busy panel's progress bar (01 §10.1).
//
// While the panel is busy it paints like any QProgressBar, so the native style
// may run its moving glow — one more sign of life during a search. While the
// panel is idle the bar must be completely still: nothing is being processed.
// The "windowsvista" style animates a glow over every bar with
// 0 < value < maximum, and after End the bar rests at 1000 of 1001 (success) or
// part-filled (failure), so idle painting draws a FULL bar — for which the
// style stops, and never starts, its animation — clipped to the real fill width.

#include <QProgressBar>

namespace wizz::app {

class PanelProgressBar final : public QProgressBar {
    Q_OBJECT

public:
    explicit PanelProgressBar(QWidget* parent = nullptr);

    /// Live (busy): normal painting, the style may animate. Not live (idle): still.
    void set_live(bool live);
    [[nodiscard]] bool is_live() const noexcept { return live_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool live_ = false;
};

} // namespace wizz::app
