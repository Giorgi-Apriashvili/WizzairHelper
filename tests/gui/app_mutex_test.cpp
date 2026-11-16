// AppMutex (D-36): the named mutex the installer's AppMutex directive looks for. The
// lifetime tests use a name of their own, so a WizzairHelper.exe running on the same
// desktop cannot disturb them.

#include "app_mutex.hpp"

#include <windows.h>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

namespace {

using wizz::app::AppMutex;

[[nodiscard]] std::wstring test_mutex_name(const wchar_t* suffix)
{
    return L"WizzairHelper.Test." + std::to_wstring(GetCurrentProcessId()) + L"." + suffix;
}

/// What Inno Setup's AppMutex check does: open the mutex by name.
[[nodiscard]] bool mutex_exists(const std::wstring& name)
{
    const HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
    if (handle == nullptr) {
        return false;
    }
    CloseHandle(handle);
    return true;
}

TEST(AppMutex, D_36_Name_is_the_installer_s_AppMutex)
{
    EXPECT_EQ(wizz::app::kAppMutexName, L"WizzairHelper.AppMutex");
}

TEST(AppMutex, D_36_Exists_while_held_and_disappears_with_the_last_holder)
{
    const std::wstring name = test_mutex_name(L"lifetime");
    ASSERT_FALSE(mutex_exists(name));
    {
        const AppMutex mutex(name);
        EXPECT_TRUE(mutex.held());
        EXPECT_TRUE(mutex_exists(name));
    }
    EXPECT_FALSE(mutex_exists(name));
}

TEST(AppMutex, D_36_A_second_copy_shares_the_mutex_and_keeps_running)
{
    const std::wstring name = test_mutex_name(L"shared");
    auto first = std::make_optional<AppMutex>(name);
    {
        const AppMutex second(name);  // another WizzairHelper.exe: no single-instance lock
        EXPECT_TRUE(first->held());
        EXPECT_TRUE(second.held());
    }
    EXPECT_TRUE(mutex_exists(name));  // the first copy still runs
    first.reset();
    EXPECT_FALSE(mutex_exists(name));
}

TEST(AppMutex, D_36_A_name_windows_refuses_is_not_an_error)
{
    // A waitable event of the same name: CreateMutexW fails with ERROR_INVALID_HANDLE.
    const std::wstring name = test_mutex_name(L"taken");
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
    ASSERT_NE(event, nullptr);
    {
        const AppMutex mutex(name);
        EXPECT_FALSE(mutex.held());
    }
    CloseHandle(event);
}

} // namespace
