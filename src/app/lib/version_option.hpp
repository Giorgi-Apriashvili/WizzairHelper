#pragma once
// `WizzairHelper.exe --version` (DECISIONS D-35, a small extension of the 01 §17.1
// command line): prints exactly "WizzairHelper X.Y.Z" and a line feed to standard
// output and exits with 0 before anything else starts (no window, no settings, no
// network, no AppMutex). X.Y.Z is WIZZ_VERSION_STRING, the project(VERSION) of
// CMakeLists.txt. Release checks and updaters read it, e.g. from PowerShell:
//
//     $(& .\WizzairHelper.exe --version 2>&1)      ->  WizzairHelper 1.2.3
//
// WizzairHelper.exe is a GUI-subsystem program, so it has no console of its own:
// the text goes to the standard output handle it inherited (the pipe or file of a
// caller that captures or redirects it) and, when there is none, to the console of
// the parent process (cmd.exe, PowerShell), which shows it after its prompt.

#include <span>
#include <string>
#include <string_view>

namespace wizz::app {

/// Whether the command-line arguments (UTF-8, without the program path) ask for the
/// version: an argument equal to "--version" anywhere, matched exactly and
/// case-sensitively like the 01 §17.1 flags. It wins over every other flag.
[[nodiscard]] bool asks_for_version(std::span<const std::string> arguments) noexcept;

/// "WizzairHelper X.Y.Z\n".
[[nodiscard]] std::string version_text();

/// Writes `text` to the standard output handle the process inherited; when there is
/// none or writing to it fails, to the console of the parent process. Returns false
/// when the text could be written to neither.
bool write_standard_output(std::string_view text) noexcept;

} // namespace wizz::app
