// The installer contract's AppMutex (D-47) and the installer launcher's failure
// path. No installer is ever started here: the launcher test names a program that
// does not exist.

#include "app_mutex.hpp"
#include "grid_actions.hpp"
#include "installer_launcher.hpp"

#include "wizz/services/updater.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

using wizz::app::AppMutex;

/// A name no other test or running app uses.
[[nodiscard]] std::wstring unique_name()
{
    return std::format(L"WizzairHelper.AppMutex.Test.{}.{}", GetCurrentProcessId(), GetTickCount64());
}

TEST(AppMutex, Exists_while_held_and_disappears_with_the_last_handle)
{
    const std::wstring name = unique_name();
    EXPECT_FALSE(AppMutex::exists(name));
    {
        AppMutex first{name};
        EXPECT_TRUE(first.held());
        EXPECT_EQ(first.name(), name);
        EXPECT_TRUE(AppMutex::exists(name)) << "what the installer's AppMutex check sees";

        AppMutex second{name};  // another running copy
        EXPECT_TRUE(second.held());
        first.release();
        EXPECT_FALSE(first.held());
        first.release();  // idempotent
        EXPECT_TRUE(AppMutex::exists(name)) << "another copy still runs";
    }
    EXPECT_FALSE(AppMutex::exists(name)) << "released when the app exits";
}

TEST(AppMutex, The_app_uses_the_contract_name)
{
    EXPECT_EQ(wizz::update::kAppMutexName, L"WizzairHelper.AppMutex");
}

TEST(InstallerLauncher, Reports_the_system_message_when_the_installer_cannot_start)
{
    const std::filesystem::path missing = std::filesystem::temp_directory_path() / (unique_name() + L"-setup.exe");
    ASSERT_FALSE(std::filesystem::exists(missing));
    const wizz::update::InstallerCommand command =
        wizz::update::silent_install_command(missing, wizz::update::Relaunch::no);
    const std::expected<void, std::string> launched = wizz::app::launch_detached(command);
    ASSERT_FALSE(launched.has_value());
    EXPECT_EQ(launched.error(), wizz::app::system_message(ERROR_FILE_NOT_FOUND).toStdString());
}

} // namespace
