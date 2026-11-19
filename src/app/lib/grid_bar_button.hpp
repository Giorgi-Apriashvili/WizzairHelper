#pragma once
// A flat action button at the right end of a result grid's top bar (DECISIONS
// D-72): "Homes on Airbnb" above the Outbound, Return and Round trips tables.
//
//   Show: [Cheapest per destination ▾]  12 fares · …                 (⌂) Homes on Airbnb
//
// Look (D-53), like the Settings button (D-63): an icon-font glyph in the accent
// colour, the text in the secondary text colour (the text colour under the mouse),
// the style's flat-button fill under the mouse and while pressed, and the accent
// focus ring after a keyboard focus change; greyed while disabled. In High Contrast
// the plain native style draws the face and the focus rectangle.
//
// Costs no height: it is exactly as tall as a check box in its font (the Round trips
// bar holds only check boxes, and 14 round trips must stay in view at the default
// window, D-53). Its width is its glyph, text and padding; minimumSizeHint() is its
// sizeHint() so it is never squeezed. A QPushButton: Space, Enter (the main window
// clicks a focused push button) and a click press it.

#include <QMargins>
#include <QPushButton>
#include <QString>

namespace wizz::app {

class GridBarButton final : public QPushButton {
    Q_OBJECT

public:
    /// Left and right padding around the glyph and the text.
    static constexpr QMargins kPadding{8, 0, 8, 0};
    /// Between the glyph and the text.
    static constexpr int kGlyphGap = 6;
    /// The glyph's point size in the icon font (the card icons are 10 pt).
    static constexpr qreal kGlyphPointSize = 10.0;

    /// `text` is also the accessible name; `tool_tip` is plain text.
    GridBarButton(char16_t glyph, const QString& text, const QString& tool_tip, QWidget* parent = nullptr);

    /// Glyph, text and padding wide; as tall as a check box in this font.
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }

    /// The colours it paints with in its current state (tests).
    [[nodiscard]] QColor glyph_colour() const;
    [[nodiscard]] QColor text_colour() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] int glyph_width() const;

    char16_t glyph_;
};

} // namespace wizz::app
