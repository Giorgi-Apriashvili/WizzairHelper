#pragma once
// Where an exclusive radio group takes the focus with Tab (DECISIONS D-75): on the selected button,
// as in a Windows radio group, or while one of its buttons has the focus, on that one, so Tab and
// Shift+Tab leave the group at once also from a button the focus reached without selecting it (a
// right-click, a press dragged off).
//
// Qt alone gives the stop to the button that last had the focus, and to every button until one had
// it (QAbstractButton's fixFocusPolicy runs on focus-in only), so Tab into a group whose second or
// third button is selected first lands on its first, unselected one. RadioTabStop sets the buttons'
// focus policies after every change of the selection and whenever one of them loses the focus; Qt
// itself makes a button the stop when it gains the focus. The arrow keys keep Qt's behaviour: they
// move to the next button and select it.

#include <QObject>

#include <vector>

class QAbstractButton;

namespace wizz::app {

class RadioTabStop final : public QObject {
public:
    /// Keeps the stop of `buttons` (checkable, one exclusive group), now and from then on; `parent`
    /// owns it (a widget that outlives the buttons, or one of their parents).
    RadioTabStop(std::vector<QAbstractButton*> buttons, QObject* parent);

    /// Gives the stop to the focused button, else to the checked one (StrongFocus; the others
    /// ClickFocus, so a click still focuses them).
    void update();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    std::vector<QAbstractButton*> buttons_;
};

} // namespace wizz::app
