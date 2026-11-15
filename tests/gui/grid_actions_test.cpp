// GridActions::system() (01 §8.8, §18.1): OpenUrl shell-executes the link, and
// when that fails the Warning box shows the system's own error message — the
// predecessor's Win32Exception.Message — never a made-up sentence. The failure
// is provoked with a file that does not exist, so no browser and no Windows
// dialog ever opens.

#include "grid_actions.hpp"

#include "wizz/ui/strings.hpp"

#include <QDir>
#include <QMessageBox>
#include <QString>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <windows.h>

#include <expected>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace wizz;

/// The system's text for a Win32 error code, trailing line break and blanks removed.
[[nodiscard]] std::string system_message(DWORD code)
{
    wchar_t buffer[512]{};
    DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buffer,
                                  static_cast<DWORD>(std::size(buffer)), nullptr);
    while (length > 0 && buffer[length - 1] <= L' ') {
        --length;
    }
    return QString::fromWCharArray(buffer, static_cast<qsizetype>(length)).toStdString();
}

/// A file name in an empty temporary directory: shell-executing it fails with ERROR_FILE_NOT_FOUND.
struct MissingFile {
    QTemporaryDir dir;
    QString path = QDir::toNativeSeparators(dir.filePath(QStringLiteral("missing-booking.html")));
};

TEST(GridActions, A_failed_open_returns_the_system_error_message)
{
    const MissingFile missing;
    ASSERT_TRUE(missing.dir.isValid());
    const std::string expected = system_message(ERROR_FILE_NOT_FOUND);
    ASSERT_FALSE(expected.empty());

    const std::expected<void, QString> opened = app::GridActions::system().open_url(missing.path);
    ASSERT_FALSE(opened.has_value());
    EXPECT_EQ(opened.error().toStdString(), expected);
}

TEST(GridActions, The_browser_warning_box_shows_the_system_error_message_and_the_link)
{
    const MissingFile missing;
    ASSERT_TRUE(missing.dir.isValid());
    app::GridActions actions = app::GridActions::system();
    std::vector<std::pair<QMessageBox::Icon, QString>> messages;
    actions.show_message = [&messages](QMessageBox::Icon icon, const QString& text) {
        messages.emplace_back(icon, text);
    };

    const std::string url = missing.path.toStdString();
    const std::expected<void, QString> opened = app::open_url_or_warn(actions, url);
    ASSERT_FALSE(opened.has_value());
    ASSERT_EQ(messages.size(), 1U);
    EXPECT_EQ(messages[0].first, QMessageBox::Warning);
    EXPECT_EQ(messages[0].second.toStdString(),
              "Could not open the browser:\n" + system_message(ERROR_FILE_NOT_FOUND) + "\n\n" + url);
}

} // namespace
