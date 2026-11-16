#pragma once
// The busy panel's progress bar (01 §10.1).
//
// Prototype "layout" look: the bar paints itself, not through the style — a
// 6 px rounded track and a rounded fill in set_fill()'s colour (the accent
// while busy, the softened outcome colour at rest). Nothing in it animates on
// its own, so an idle bar is completely still (D-34); only a live marquee
// (range 0..0) moves, one step per repaint, which the busy panel's 40 ms
// timer drives while it is busy.

#include <QColor>
#include <QElapsedTimer>
#include <QProgressBar>

namespace wizz::app {

class PanelProgressBar final : public QProgressBar {
    Q_OBJECT

public:
    explicit PanelProgressBar(QWidget* parent = nullptr);

    /// Live (busy): the marquee segment may move. Not live (idle): still.
    void set_live(bool live);
    [[nodiscard]] bool is_live() const noexcept { return live_; }
    /// The colour of the filled part.
    void set_fill(const QColor& colour);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    bool live_ = false;
    QColor fill_;
    QElapsedTimer clock_;
};

} // namespace wizz::app
