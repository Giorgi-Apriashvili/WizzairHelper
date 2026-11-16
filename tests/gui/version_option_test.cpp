// `--version` (D-35): which command lines ask for it, the exact text, and that it reaches
// the standard output handle a caller hands the process (a pipe when PowerShell or a
// script captures it, a file when it is redirected). The real WizzairHelper.exe is run
// with --version by the CTest tests app.version and app.version.powershell.

#include "version_option.hpp"

#include <QScopeGuard>

#include <windows.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace {

using wizz::app::asks_for_version;
using wizz::app::version_text;
using wizz::app::write_standard_output;

[[nodiscard]] bool asks(std::vector<std::string> arguments)
{
    return asks_for_version(arguments);
}

TEST(VersionOption, D_35_Is_the_exact_case_sensitive_flag_anywhere_on_the_command_line)
{
    EXPECT_TRUE(asks({"--version"}));
    EXPECT_TRUE(asks({"--autotest", "out", "--version"}));
    EXPECT_TRUE(asks({"--version", "--autotest", "out"}));

    EXPECT_FALSE(asks({}));
    EXPECT_FALSE(asks({"--autotest", "out"}));
    EXPECT_FALSE(asks({"--Version"}));
    EXPECT_FALSE(asks({"-version"}));
    EXPECT_FALSE(asks({"-v"}));
    EXPECT_FALSE(asks({"/version"}));
    EXPECT_FALSE(asks({"--version=1"}));
    EXPECT_FALSE(asks({" --version"}));
    EXPECT_FALSE(asks({"version"}));
}

TEST(VersionOption, D_35_Text_is_the_product_name_and_the_project_version_and_a_line_feed)
{
    EXPECT_EQ(version_text(), std::string("WizzairHelper ") + WIZZ_VERSION_STRING + "\n");
    EXPECT_TRUE(std::regex_match(version_text(), std::regex(R"(WizzairHelper \d+\.\d+\.\d+\n)")))
        << version_text();
}

/// Points the process's standard output handle at `handle` until the guard ends.
[[nodiscard]] auto redirect_standard_output(HANDLE handle)
{
    const HANDLE previous = GetStdHandle(STD_OUTPUT_HANDLE);
    EXPECT_TRUE(SetStdHandle(STD_OUTPUT_HANDLE, handle));
    return qScopeGuard([previous] { SetStdHandle(STD_OUTPUT_HANDLE, previous); });
}

TEST(VersionOption, D_35_Writes_to_an_inherited_pipe_as_when_powershell_captures_the_output)
{
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    ASSERT_TRUE(CreatePipe(&read_end, &write_end, nullptr, 0));
    const auto close_read = qScopeGuard([read_end] { CloseHandle(read_end); });
    {
        const auto close_write = qScopeGuard([write_end] { CloseHandle(write_end); });
        const auto restore = redirect_standard_output(write_end);
        EXPECT_TRUE(write_standard_output("WizzairHelper 9.8.7\n"));
    }

    std::string received;
    char buffer[256];
    DWORD count = 0;
    while (ReadFile(read_end, buffer, sizeof buffer, &count, nullptr) && count > 0) {
        received.append(buffer, count);
    }
    EXPECT_EQ(received, "WizzairHelper 9.8.7\n");  // byte for byte: no CR added, no BOM
}

TEST(VersionOption, D_35_Writes_to_an_inherited_file_as_when_the_output_is_redirected)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("wizz_version_" + std::to_string(GetCurrentProcessId()) + ".txt");
    const auto remove_file = qScopeGuard([&path] {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    });
    {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(file, INVALID_HANDLE_VALUE);
        const auto close_file = qScopeGuard([file] { CloseHandle(file); });
        const auto restore = redirect_standard_output(file);
        EXPECT_TRUE(write_standard_output(version_text()));
    }

    std::ifstream in(path, std::ios::binary);
    const std::string written{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    EXPECT_EQ(written, version_text());
}

} // namespace
