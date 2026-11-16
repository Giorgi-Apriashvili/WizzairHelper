#pragma once
// The Log tab (01 §12.1): a read-only multi-line text box with both scroll bars,
// no word wrap, Cascadia Mono 9.5 pt (Consolas on Windows 10) in the theme's text
// colour on its surface colour (ui_style.hpp; never the grey read-only colour,
// whatever the Qt style paints), no frame of its own: the results card frames it.
// While it has the keyboard focus, a 3 px accent bar runs down its left edge (the
// grids mark their selected row the same way). The colours follow the look.
// append_line writes "[" + local now "HH:mm:ss" (culture time separator) + "] "
// + line + CRLF through wizz::ui::log_entry, moves the caret to the end and
// scrolls there. There is no size limit; the log lives in memory only.
//
// text() is the box's text as the .NET TextBox held it (CRLF line ends), which
// the --autotest summary.json splits into lines (ui::split_log).
// UI thread only.

#include "wizz/core/locale.hpp"
#include "wizz/core/time.hpp"

#include <QPlainTextEdit>

#include <memory>
#include <string>
#include <string_view>

namespace wizz::app {

class LogView final : public QPlainTextEdit {
    Q_OBJECT

public:
    /// `clock` must outlive the view (local time of the line prefix); `locale` gives the
    /// culture time separator.
    LogView(const Clock& clock, std::shared_ptr<const Locale> locale, QWidget* parent = nullptr);

    /// `Log(line)`: appends "[HH:mm:ss] {line}" + CRLF and scrolls to the end.
    void append_line(std::string_view line);

    /// Everything appended so far, CRLF line ends included.
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

    /// Cascadia Mono 9.5 pt, else Consolas.
    [[nodiscard]] static QFont log_font();

protected:
    bool event(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void apply_system_colours();

    const Clock& clock_;
    std::shared_ptr<const Locale> locale_;
    std::string text_;
};

} // namespace wizz::app
