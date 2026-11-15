#include "log_view.hpp"

#include "system_colours.hpp"

#include "wizz/ui/leg_text.hpp"

#include <QEvent>
#include <QFont>
#include <QPalette>
#include <QScrollBar>
#include <QString>
#include <QTextCursor>

#include <stdexcept>
#include <utility>

namespace wizz::app {

LogView::LogView(const Clock& clock, std::shared_ptr<const Locale> locale, QWidget* parent)
    : QPlainTextEdit(parent), clock_(clock), locale_(std::move(locale))
{
    if (!locale_) {
        throw std::invalid_argument("LogView needs a locale.");
    }
    setReadOnly(true);
    setUndoRedoEnabled(false);  // read-only and unlimited: no undo history to keep
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setWordWrapMode(QTextOption::NoWrap);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);  // ScrollBars.Both
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setFont(log_font());
    apply_system_colours();
}

QFont LogView::log_font()
{
    QFont font(QStringLiteral("Courier New"));
    font.setPointSizeF(9.0);
    font.setStyleHint(QFont::TypeWriter);
    font.setFixedPitch(true);
    return font;
}

void LogView::append_line(std::string_view line)
{
    const std::string entry = ui::log_entry(clock_.local_now(), line, *locale_);
    text_ += entry;

    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    // The document stores paragraphs: a CRLF line end becomes one block separator.
    cursor.insertText(QString::fromStdString(entry).replace(QStringLiteral("\r\n"), QStringLiteral("\n")));
    setTextCursor(cursor);  // caret at the end (AppendText)
    ensureCursorVisible();  // ScrollToCaret: also brings the view back to column 0
    QScrollBar* vertical = verticalScrollBar();
    vertical->setValue(vertical->maximum());  // and all the way down
}

bool LogView::event(QEvent* event)
{
    const bool handled = QPlainTextEdit::event(event);
    switch (event->type()) {
    case QEvent::Polish:
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
    case QEvent::ThemeChange:
        apply_system_colours();  // after the style has polished the viewport
        break;
    default:
        break;
    }
    return handled;
}

void LogView::apply_system_colours()
{
    // BackColor = SystemColors.Window, also while read-only, and its WindowText. Styles
    // may leave the viewport unfilled (the "windows11" style draws a grey panel behind it).
    QPalette colours = palette();
    const QColor window = system_colour(SystemColour::window);
    const QColor window_text = system_colour(SystemColour::window_text);
    if (colours.color(QPalette::Base) != window || colours.color(QPalette::Text) != window_text) {
        colours.setColor(QPalette::Base, window);
        colours.setColor(QPalette::Text, window_text);
        setPalette(colours);
    }
    viewport()->setBackgroundRole(QPalette::Base);
    viewport()->setAutoFillBackground(true);
}

} // namespace wizz::app
