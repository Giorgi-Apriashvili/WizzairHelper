#include "grid_actions.hpp"

#include "app_info.hpp"

#include "wizz/ui/strings.hpp"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>

#include <windows.h>

#include <string>

namespace wizz::app {

namespace {

/// QDesktopServices reports only success or failure (ShellExecute's code goes to a Qt
/// warning), so the box names the failing step instead of a Win32 message.
constexpr QStringView kBrowserFailure = u"Windows could not start the default browser for this link.";

[[nodiscard]] std::expected<void, QString> open_with_desktop_services(const QUrl& url)
{
    if (!url.isValid()) {
        return std::unexpected(QStringLiteral("The link is not a valid address."));
    }
    if (!QDesktopServices::openUrl(url)) {
        return std::unexpected(kBrowserFailure.toString());
    }
    return {};
}

/// Qt's own clipboard writer sleeps between its retries and cannot report failure, so probe
/// the clipboard first (OpenClipboard never blocks) and verify the result afterwards.
[[nodiscard]] bool try_set_system_clipboard(const QString& text)
{
    if (OpenClipboard(nullptr) == FALSE) {
        return false;  // another program holds it
    }
    CloseClipboard();
    QClipboard* clipboard = QGuiApplication::clipboard();
    clipboard->setText(text);
    return clipboard->text() == text;
}

void show_unowned_message(QMessageBox::Icon icon, const QString& text)
{
    QMessageBox box{icon, product_name(), text, QMessageBox::Ok};  // no owner (01 §18.1)
    box.exec();
}

} // namespace

GridActions GridActions::system()
{
    return GridActions{
        .open_url = open_with_desktop_services,
        .try_set_clipboard = try_set_system_clipboard,
        .show_message = show_unowned_message,
    };
}

std::expected<void, QString> open_url_or_warn(const GridActions& actions, std::string_view url)
{
    const QString text = QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size()));
    std::expected<void, QString> opened = actions.open_url ? actions.open_url(QUrl{text})
                                                           : std::expected<void, QString>{};
    if (!opened && actions.show_message) {
        actions.show_message(QMessageBox::Warning,
                             QString::fromStdString(ui::strings::browser_error(opened.error().toStdString(), url)));
    }
    return opened;
}

} // namespace wizz::app
