#pragma once
// Helpers for the input-panel widget tests: the fixed start-up day, synthetic
// keyboard / focus / mouse events (QtTest is not part of the dependency set),
// and optional PNG renders for visual inspection.
//
// Renders are written only when WIZZ_GUI_RENDER_DIR names a directory (run the
// test binary by hand, without QT_QPA_PLATFORM=minimal, to get real fonts).

#include "wizz/core/time.hpp"

#include <QApplication>
#include <QDir>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QString>
#include <QWidget>

#include <optional>

namespace wizz::test::gui {

/// The suite's fixed "today" (ARCHITECTURE.md: 2026-11-02, a Monday).
inline constexpr Date kToday = Date::from_ymd(2026, 11, 2);

[[nodiscard]] inline std::optional<QString> render_dir()
{
    const QString dir = qEnvironmentVariable("WIZZ_GUI_RENDER_DIR");
    if (dir.isEmpty() || !QDir(dir).exists()) {
        return std::nullopt;
    }
    return dir;
}

/// Saves `widget.grab()` as `<WIZZ_GUI_RENDER_DIR>/<name>.png` when enabled.
inline void save_render(QWidget& widget, const QString& name)
{
    if (const std::optional<QString> dir = render_dir()) {
        widget.grab().save(QDir(*dir).filePath(name + QStringLiteral(".png")));
    }
}

/// Press + release of one key; returns whether the press was accepted.
inline bool send_key(QWidget* target, int key, const QString& text = {},
                     Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QApplication::sendEvent(target, &release);
    return press.isAccepted();
}

/// Types printable ASCII text key by key.
inline void type_text(QWidget* target, const QString& text)
{
    for (const QChar c : text) {
        send_key(target, static_cast<int>(c.toUpper().unicode()), QString(c));
    }
}

inline void send_focus_in(QWidget* target, Qt::FocusReason reason = Qt::TabFocusReason)
{
    QFocusEvent event(QEvent::FocusIn, reason);
    QApplication::sendEvent(target, &event);
}

inline void send_double_click(QWidget* target, QPoint position)
{
    const QPointF local(position);
    const QPointF global(target->mapToGlobal(position));
    QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(target, &release);
    QMouseEvent dbl(QEvent::MouseButtonDblClick, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &dbl);
    QMouseEvent release2(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(target, &release2);
}

} // namespace wizz::test::gui
