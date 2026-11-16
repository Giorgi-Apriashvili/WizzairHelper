#pragma once
// Starting the update installer (D-47). The command is the contract's silent
// command line (update::silent_install_command); the app never waits for it.

#include "wizz/services/updater.hpp"

#include <expected>
#include <functional>
#include <string>

namespace wizz::app {

/// Starts `command` and returns at once; on failure the system's message for it.
/// Injectable, so tests never start a real installer.
using InstallerLauncher = std::function<std::expected<void, std::string>(const update::InstallerCommand& command)>;

/// The real launcher: CreateProcessW with the program as lpApplicationName and
/// command.command_line() (the program quoted) as the command line, no inherited
/// handles, its own process group, the installer's folder as the working directory
/// (so the install folder is not held open), outside the app's job object when that
/// is allowed; both handles are closed at once. When Windows says the program needs
/// elevation, ShellExecuteExW starts it with command.parameters() instead (UAC).
[[nodiscard]] std::expected<void, std::string> launch_detached(const update::InstallerCommand& command);

} // namespace wizz::app
