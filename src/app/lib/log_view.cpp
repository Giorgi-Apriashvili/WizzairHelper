#include "log_view.hpp"

#include "modern_style.hpp"
#include "ui_style.hpp"

#include "wizz/ui/leg_text.hpp"

#include <QEvent>
#include <QFont>
#include <QPainter>
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
    setFrameShape(QFrame::NoFrame);  // the results card is the frame
    setCornerWidget(ModernStyle::make_surface_corner(this));  // where the scroll bars meet
    // Room for the focus bar left of the text; the document margin keeps the text clear of it.
    setViewportMargins(ModernStyle::kSelectionBarWidth, 0, 0, 0);
    document()->setDocumentMargin(8);
    apply_system_colours();
    style::on_theme_change(this, [this] { apply_system_colours(); });
}

QFont LogView::log_font()
{
    return style::mono_font(9.5);  // Cascadia Mono, Consolas on Windows 10
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
    if (event->type() == QEvent::Paint) {
        // The view's own area outside the viewport: the focus bar, or the surface.
        QPainter painter(this);
        const QRect bar(0, 0, ModernStyle::kSelectionBarWidth, viewport()->geometry().bottom() + 1);
        painter.fillRect(bar, hasFocus() ? style::theme().accent_line : style::theme().surface);
    }
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

void LogView::focusInEvent(QFocusEvent* event)
{
    QPlainTextEdit::focusInEvent(event);
    update();  // the focus bar
}

void LogView::focusOutEvent(QFocusEvent* event)
{
    QPlainTextEdit::focusOutEvent(event);
    update();
}

void LogView::apply_system_colours()
{
    // BackColor = SystemColors.Window, also while read-only, and its WindowText. Styles
    // may leave the viewport unfilled (the "windows11" style draws a grey panel behind it).
    QPalette colours = palette();
    const QColor window = style::theme().surface;
    const QColor window_text = style::theme().text;
    if (colours.color(QPalette::Base) != window || colours.color(QPalette::Text) != window_text) {
        colours.setColor(QPalette::Base, window);
        colours.setColor(QPalette::Text, window_text);
        setPalette(colours);
    }
    viewport()->setBackgroundRole(QPalette::Base);
    viewport()->setAutoFillBackground(true);
}

} // namespace wizz::app
