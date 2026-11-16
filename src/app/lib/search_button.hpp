#pragma once
// "Search outbound" / "Search return": the primary action buttons (DECISIONS
// D-53). A rounded accent (#C5006E) button with white semibold text and
// an optional icon glyph before the text; lighter under the mouse, darker while
// pressed. While disabled it looks like every disabled control: the disabled
// input fill, an input-border outline and the disabled text colour, so it never
// reads as a pressed accent button. Keyboard focus shows as a white ring inside
// the face. Its height
// is the height of every other input (ModernStyle::control_height), so it lines
// up with the fields next to it. High Contrast paints it in the system
// Highlight / HighlightedText, and a disabled one as a GrayText outline.
//
// A focused button is clicked by Enter as well as Space (01 §6.3), so Enter on a
// search button never presses the window's other AcceptButton.

#include <QPushButton>
#include <QSize>

namespace wizz::app {

class SearchButton final : public QPushButton {
    Q_OBJECT

public:
    /// Padding (left, top, right, bottom) around the icon and text.
    static constexpr QMargins kPadding{16, 2, 16, 2};
    /// Never smaller than this.
    static constexpr QSize kMinimumSize{75, 23};

    explicit SearchButton(const QString& text, QWidget* parent = nullptr);

    /// An icon-font glyph drawn before the text (0 = none).
    void set_glyph(char16_t glyph);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

    /// The colours the button paints with in its current state.
    [[nodiscard]] QColor background_color() const;
    [[nodiscard]] QColor text_color() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    [[nodiscard]] int glyph_width() const;

    char16_t glyph_ = 0;
};

} // namespace wizz::app
