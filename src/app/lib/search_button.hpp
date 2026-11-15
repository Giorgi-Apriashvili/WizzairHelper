#pragma once
// "Search outbound" / "Search return" (01 §4.5 StyleSearchButton): bold form
// font (Segoe UI 9 pt bold), padding (8, 2, 8, 2), Wizz magenta #C5006E
// background, white text, flat, no border, no visual-style background. While
// disabled the background stays magenta and the text takes a lighter tint
// (01 §4.5 OPEN QUESTION). Like a WinForms AutoSize button (AutoSizeMode
// GrowOnly) it is never smaller than the default 75 × 23 button.
//
// A focused button is clicked by Enter as well as Space (01 §6.3), so Enter on a
// search button never presses the window's other AcceptButton.

#include <QPushButton>
#include <QSize>

namespace wizz::app {

class SearchButton final : public QPushButton {
    Q_OBJECT

public:
    /// Padding (left, top, right, bottom) around the text.
    static constexpr QMargins kPadding{8, 2, 8, 2};
    /// WinForms Button default size; AutoSize GrowOnly never shrinks below it.
    static constexpr QSize kMinimumSize{75, 23};

    explicit SearchButton(const QString& text, QWidget* parent = nullptr);

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

    /// The colours the button paints with in its current state.
    [[nodiscard]] QColor background_color() const;
    [[nodiscard]] QColor text_color() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
};

} // namespace wizz::app
