#pragma once
// A row of joined segments of which one is selected (DECISIONS D-75): the Settings window's
// Theme switch, "Auto | Dark | Light", beside Close.
//
//   ╭──────┬──────┬───────╮
//   │ Auto │ Dark │ Light │   the selected segment: the selection tint, its label in the text
//   ╰──────┴─▀▀▀▀─┴───────╯   colour, a 3 px accent pill along its bottom (like a selected tab)
//
// Each segment is a QRadioButton painted as part of the row, so the row behaves like the
// window's other radio group and like a Windows radio group: one Tab stop, the selected segment
// (also before any segment had the focus; Qt alone would stop on the first one until then), or
// while a segment has the focus that one (a right-click or a press dragged off focuses without
// selecting), so Tab leaves the switch at once; the arrow keys move to the next segment and select
// it; Space or a click selects; a screen reader hears "Dark, radio button, checked". The segments
// are auto-exclusive siblings in the switch.
//
// Look (D-53): rounded like the inputs and buttons (ModernStyle::kRadius), the secondary button's
// fill inside the input frame, a 1 px divider between two unselected segments, the button's hover
// and pressed fills on an unselected segment, labels in the secondary text colour (the text colour
// under the mouse), the style's focus ring inside the focused segment after a keyboard focus
// change. In High Contrast the tokens are the system colours (Button, WindowText, Highlight with
// HighlightedText for the selected segment, without the pill) and the focus is the plain style's
// dotted focus rectangle, in the inverse of the segment's fill. Colours are read from the theme
// when a segment paints.
//
// Every segment is as wide as the widest label plus kPadding on each side, and the row is as tall
// as the style makes a push button (one control height in the app's look, ModernStyle::
// control_height; the native height in High Contrast), so it matches a push button beside it.

#include <QRadioButton>
#include <QStringList>
#include <QWidget>

#include <vector>

namespace wizz::app {

class SegmentedSwitch final : public QWidget {
    Q_OBJECT

public:
    /// Left and right of the widest label, in every segment.
    static constexpr int kPadding = 14;
    /// The accent pill under the selected segment's label: its height and its inset from the
    /// segment's left and right edges.
    static constexpr int kPillHeight = 3;
    static constexpr int kPillInset = 10;

    /// One segment per label, left to right; the first is selected.
    explicit SegmentedSwitch(const QStringList& labels, QWidget* parent = nullptr);

    [[nodiscard]] int count() const noexcept { return static_cast<int>(segments_.size()); }
    [[nodiscard]] QRadioButton* segment(int index) const;
    /// The selected segment's index.
    [[nodiscard]] int current() const;
    /// Selects segment `index` (ignored when out of range); current_changed() follows when it
    /// was not selected.
    void set_current(int index);

    /// The colours a segment paints with in its current state (tests).
    [[nodiscard]] QColor fill_colour(int index) const;
    [[nodiscard]] QColor label_colour(int index) const;

Q_SIGNALS:
    /// Another segment was selected, by the user or by set_current().
    void current_changed(int index);

private:
    class Segment;
    friend class Segment;

    /// Only one segment takes the focus with Tab: the focused one, else the selected one.
    void keep_tab_stop_on_current();

    std::vector<Segment*> segments_;
};

} // namespace wizz::app
