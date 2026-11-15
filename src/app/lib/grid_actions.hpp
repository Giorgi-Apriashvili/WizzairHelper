#pragma once
// Side effects of the result grids' actions (01 §8.8, §8.9, §9.4, §18.1):
// opening a booking URL in the default browser, writing the clipboard and the
// unowned MessageBoxes. They are injectable so widget tests never launch a
// browser, touch the user's clipboard or block in a modal box.

#include <QMessageBox>
#include <QString>

#include <chrono>
#include <expected>
#include <functional>
#include <string_view>

namespace wizz::app {

struct GridActions {
    /// Shell-executes `url`, so the default browser opens it. The error text is what
    /// follows "Could not open the browser:\n" in the Warning box.
    std::function<std::expected<void, QString>(const QString& url)> open_url;
    /// One non-blocking attempt to put `text` on the clipboard; false while another
    /// program holds the clipboard (the grid retries, 01 §8.9).
    std::function<bool(const QString& text)> try_set_clipboard;
    /// An unowned MessageBox: caption "WizzairHelper", the icon, an OK button (01 §18.1).
    std::function<void(QMessageBox::Icon icon, const QString& text)> show_message;

    /// ShellExecuteExW reporting the system's message for a failure (the predecessor's
    /// Win32Exception.Message), the system clipboard and QMessageBox.
    [[nodiscard]] static GridActions system();
};

/// "WinForms retries the clipboard internally (about 10 × 100 ms)" (01 §8.9): the number of
/// attempts and the pause between two of them (a timer, never a sleep on the UI thread).
inline constexpr int kClipboardAttempts = 10;
inline constexpr std::chrono::milliseconds kClipboardRetryDelay{100};

/// `OpenUrl(url)` (01 §8.8): opens `url` through `actions`; on failure shows the Warning box
/// "Could not open the browser:\n{error}\n\n{url}" and returns the error.
std::expected<void, QString> open_url_or_warn(const GridActions& actions, std::string_view url);

} // namespace wizz::app
