#include "grid_actions.hpp"

#include "app_info.hpp"

#include "wizz/ui/strings.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QScopeGuard>

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <thread>

namespace wizz::app {

namespace {

/// The Win32 error behind a ShellExecute SE_ERR_* result (.NET's Process.GetShellError).
[[nodiscard]] DWORD shell_error(HINSTANCE result) noexcept
{
    const auto code = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(result));
    switch (code) {
    case SE_ERR_FNF:
        return ERROR_FILE_NOT_FOUND;
    case SE_ERR_PNF:
        return ERROR_PATH_NOT_FOUND;
    case SE_ERR_ACCESSDENIED:
        return ERROR_ACCESS_DENIED;
    case SE_ERR_OOM:
        return ERROR_NOT_ENOUGH_MEMORY;
    case SE_ERR_DDEFAIL:
    case SE_ERR_DDEBUSY:
    case SE_ERR_DDETIMEOUT:
        return ERROR_DDE_FAIL;
    case SE_ERR_SHARE:
        return ERROR_SHARING_VIOLATION;
    case SE_ERR_NOASSOC:
        return ERROR_NO_ASSOCIATION;
    case SE_ERR_DLLNOTFOUND:
        return ERROR_DLL_NOT_FOUND;
    default:
        return code;
    }
}

/// Win32Exception.Message: the system's text for `code` without its trailing line break.
[[nodiscard]] QString system_message(DWORD code)
{
    LPWSTR buffer = nullptr;
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    const auto release = qScopeGuard([buffer] { LocalFree(buffer); });
    QString text = QString::fromWCharArray(buffer, static_cast<qsizetype>(length));
    while (!text.isEmpty() && text.back().unicode() <= u' ') {
        text.chop(1);
    }
    if (text.isEmpty()) {
        return QStringLiteral("Unknown error (0x%1)").arg(QString::number(code, 16));
    }
    return text;
}

/// Process.Start with UseShellExecute, as the predecessor's OpenUrl: ShellExecuteExW without
/// error UI; returns the Win32 error, ERROR_SUCCESS when the target was started. The call runs
/// on its own STA thread, as QDesktopServices does, so the shell never spins the UI thread's
/// event loop.
[[nodiscard]] DWORD shell_execute(const std::wstring& target)
{
    DWORD error = ERROR_SUCCESS;
    std::jthread worker{[&target, &error] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW info{};
        info.cbSize = static_cast<DWORD>(sizeof(info));
        info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpFile = target.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&info) == FALSE) {
            error = GetLastError();
            if (error == ERROR_SUCCESS) {
                error = shell_error(info.hInstApp);
            }
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
    }};
    worker.join();
    return error;
}

[[nodiscard]] std::expected<void, QString> open_with_shell(const QString& url)
{
    if (const DWORD error = shell_execute(url.toStdWString()); error != ERROR_SUCCESS) {
        return std::unexpected(system_message(error));
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
        .open_url = open_with_shell,
        .try_set_clipboard = try_set_system_clipboard,
        .show_message = show_unowned_message,
    };
}

std::expected<void, QString> open_url_or_warn(const GridActions& actions, std::string_view url)
{
    const QString text = QString::fromUtf8(url.data(), static_cast<qsizetype>(url.size()));
    std::expected<void, QString> opened = actions.open_url ? actions.open_url(text) : std::expected<void, QString>{};
    if (!opened && actions.show_message) {
        actions.show_message(QMessageBox::Warning,
                             QString::fromStdString(ui::strings::browser_error(opened.error().toStdString(), url)));
    }
    return opened;
}

} // namespace wizz::app
