#pragma once
// The gear "Settings" button (DECISIONS D-63): the result tabs' top-right corner widget,
// at the right end of the row with Outbound / Return / Round trips / Log. It opens the
// Settings window (settings_dialog.hpp).
//
//   Outbound   Return   Round trips   Log                               (⚙) Settings
//
// Look (D-53): flat like an unselected tab — the gear glyph of the icon font in the
// accent colour (like the card icons), the text in the secondary text colour (the text
// colour under the mouse), the style's flat-button fill under the mouse and while
// pressed, and the accent focus ring of the tabs after a keyboard focus change. Colours
// are read from the theme when it paints. In High Contrast the plain native style draws
// the face and the focus rectangle, and the tokens are the system colours.
//
// Costs no height: it is exactly as tall as the tab bar (QTabWidget bounds a corner
// widget to the tab bar's height; the size hint asks for that height, so the tab widget's
// own size hints do not grow either). Its width is its glyph, text and padding. A
// QPushButton: Space, Enter (the main window clicks a focused push button) and a click
// press it; its text, accessible name and tooltip are "Settings".

#include <QMargins>
#include <QPushButton>

namespace wizz::app {

class SettingsButton final : public QPushButton {
    Q_OBJECT

public:
    /// Left and right padding around the glyph and the text.
    static constexpr QMargins kPadding{10, 0, 10, 0};
    /// Between the gear and the text.
    static constexpr int kGlyphGap = 6;
    /// The gear's point size in the icon font (the card icons are 10 pt).
    static constexpr qreal kGlyphPointSize = 10.5;
    /// Where the hover fill and the focus ring sit inside the button, as on a tab.
    static constexpr QMargins kFaceMargins{3, 4, 3, 5};
    static constexpr QMargins kFocusMargins{2, 3, 2, 4};

    explicit SettingsButton(QWidget* parent = nullptr);

    /// Glyph, text and padding wide; as tall as the parent tab widget's tab bar (minus the
    /// style's tab-bar base, which QTabWidget leaves out of a corner), else one control height.
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override { return sizeHint(); }

    /// The colours it paints with in its current state (tests).
    [[nodiscard]] QColor glyph_colour() const;
    [[nodiscard]] QColor text_colour() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] int glyph_width() const;
};

} // namespace wizz::app
