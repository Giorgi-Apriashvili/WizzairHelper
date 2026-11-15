#pragma once
// Helpers of the result-grid widget tests: a recorder for the grids' side
// effects (no browser, no system clipboard, no modal box), synthetic mouse and
// key input sent through QApplication like real input, the Tab stops of a
// window, and an event-loop wait without sleeping.

#include "grid_actions.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QTableView>
#include <QTimer>

#include <chrono>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace wizz::test::gui {

/// Records what a grid opens, copies and shows.
struct ActionRecorder {
    std::vector<QString> opened;
    std::vector<std::pair<QMessageBox::Icon, QString>> messages;
    std::vector<QString> clipboard;
    int clipboard_attempts = 0;
    bool clipboard_busy = false;          ///< every clipboard attempt fails
    std::optional<QString> open_error;    ///< every open fails with this text

    [[nodiscard]] app::GridActions actions()
    {
        return app::GridActions{
            .open_url = [this](const QString& url) -> std::expected<void, QString> {
                if (open_error) {
                    return std::unexpected(*open_error);
                }
                opened.push_back(url);
                return {};
            },
            .try_set_clipboard = [this](const QString& text) {
                ++clipboard_attempts;
                if (clipboard_busy) {
                    return false;
                }
                clipboard.push_back(text);
                return true;
            },
            .show_message = [this](QMessageBox::Icon icon, const QString& text) { messages.emplace_back(icon, text); },
        };
    }
};

/// Centre of a cell in viewport coordinates.
[[nodiscard]] inline QPoint cell_center(const QTableView& view, int row, int column)
{
    return view.visualRect(view.model()->index(row, column)).center();
}

inline void send_mouse(QWidget* target, QEvent::Type type, QPoint pos, Qt::MouseButton button)
{
    const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons{button};
    QMouseEvent event{type, QPointF{pos}, QPointF{target->mapToGlobal(pos)}, button, held, Qt::NoModifier};
    QApplication::sendEvent(target, &event);
}

/// Press and release on the table's viewport.
inline void click(QTableView& view, QPoint pos, Qt::MouseButton button = Qt::LeftButton)
{
    send_mouse(view.viewport(), QEvent::MouseButtonPress, pos, button);
    send_mouse(view.viewport(), QEvent::MouseButtonRelease, pos, button);
}

/// The Windows double-click sequence: press, release, double-click, release.
inline void double_click(QTableView& view, QPoint pos)
{
    click(view, pos);
    send_mouse(view.viewport(), QEvent::MouseButtonDblClick, pos, Qt::LeftButton);
    send_mouse(view.viewport(), QEvent::MouseButtonRelease, pos, Qt::LeftButton);
}

/// Centre of a header section in the header viewport's coordinates.
[[nodiscard]] inline QPoint header_center(const QHeaderView& header, int section)
{
    return QPoint{header.sectionViewportPosition(section) + header.sectionSize(section) / 2, header.height() / 2};
}

/// A left double-click on a header section, in the Windows sequence (see double_click).
inline void double_click_header(QHeaderView& header, int section)
{
    const QPoint pos = header_center(header, section);
    send_mouse(header.viewport(), QEvent::MouseButtonPress, pos, Qt::LeftButton);
    send_mouse(header.viewport(), QEvent::MouseButtonRelease, pos, Qt::LeftButton);
    send_mouse(header.viewport(), QEvent::MouseButtonDblClick, pos, Qt::LeftButton);
    send_mouse(header.viewport(), QEvent::MouseButtonRelease, pos, Qt::LeftButton);
}

/// Sends one key event; returns whether a receiver accepted it.
inline bool send_key(QWidget* target, QEvent::Type type, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event{type, key, modifiers};
    QApplication::sendEvent(target, &event);
    return event.isAccepted();
}

/// Key press then key release.
inline void tap_key(QWidget* target, int key)
{
    send_key(target, QEvent::KeyPress, key);
    send_key(target, QEvent::KeyRelease, key);
}

/// Types printable text: a press and a release with the character for each one.
inline void type_text(QWidget* target, const QString& text)
{
    for (const QChar character : text) {
        const int key = character.toUpper().unicode();
        QKeyEvent press{QEvent::KeyPress, key, Qt::NoModifier, QString{character}};
        QApplication::sendEvent(target, &press);
        QKeyEvent release{QEvent::KeyRelease, key, Qt::NoModifier, QString{character}};
        QApplication::sendEvent(target, &release);
    }
}

/// The widgets Tab visits in `window`, in order: Qt's focus chain filtered like
/// QWidget::focusNextPrevChild (tab focus, no focus proxy, visible, enabled).
[[nodiscard]] inline std::vector<QWidget*> tab_stops(QWidget& window)
{
    std::vector<QWidget*> stops;
    for (QWidget* widget = window.nextInFocusChain(); widget != nullptr && widget != &window;
         widget = widget->nextInFocusChain()) {
        if ((widget->focusPolicy() & Qt::TabFocus) == Qt::TabFocus && widget->focusProxy() == nullptr &&
            widget->isVisibleTo(&window) && widget->isEnabled()) {
            stops.push_back(widget);
        }
    }
    return stops;
}

/// A right-click context-menu event on the table's viewport.
inline void context_menu_at(QTableView& view, QPoint pos)
{
    QContextMenuEvent event{QContextMenuEvent::Mouse, pos, view.viewport()->mapToGlobal(pos)};
    QApplication::sendEvent(view.viewport(), &event);
}

/// Runs the event loop until `done()` or the timeout; returns `done()`.
[[nodiscard]] inline bool wait_until(const std::function<bool()>& done, std::chrono::milliseconds timeout)
{
    if (done()) {
        return true;
    }
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done()) {
            loop.quit();
        }
    });
    QTimer::singleShot(timeout, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    return done();
}

/// Lets queued layout and paint work run.
inline void settle()
{
    QApplication::processEvents();
    QApplication::processEvents();
}

} // namespace wizz::test::gui
