#include "radio_tab_stop.hpp"

#include <QAbstractButton>
#include <QEvent>

#include <algorithm>
#include <utility>

namespace wizz::app {

RadioTabStop::RadioTabStop(std::vector<QAbstractButton*> buttons, QObject* parent)
    : QObject(parent), buttons_(std::move(buttons))
{
    for (QAbstractButton* button : buttons_) {
        connect(button, &QAbstractButton::toggled, this, [this] { update(); });
        button->installEventFilter(this);
    }
    update();
}

void RadioTabStop::update()
{
    const auto focused = std::ranges::find_if(buttons_, [](const QAbstractButton* each) { return each->hasFocus(); });
    for (QAbstractButton* each : buttons_) {
        const bool stop = focused != buttons_.end() ? each == *focused : each->isChecked();
        each->setFocusPolicy(stop ? Qt::StrongFocus : Qt::ClickFocus);
    }
}

bool RadioTabStop::eventFilter(QObject* /*watched*/, QEvent* event)
{
    // A filter runs before the button's own handler, and Qt has already moved the focus on when the
    // FocusOut arrives: the stop goes back to the checked button (or to the button now focused).
    // FocusIn needs nothing: Qt's own handler makes the focused button the stop.
    if (event->type() == QEvent::FocusOut) {
        update();
    }
    return false;
}

} // namespace wizz::app
