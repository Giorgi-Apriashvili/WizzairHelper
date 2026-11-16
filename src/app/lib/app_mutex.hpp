#pragma once
// The named mutex of the installer / updater contract (D-47): WizzairHelper holds
// `WizzairHelper.AppMutex` (update::kAppMutexName) from start-up until it exits,
// so the Inno Setup installer (AppMutex=WizzairHelper.AppMutex) can tell that the
// app is running. The mutex is never owned — the installer only asks whether one of
// that name exists — and every running copy keeps a handle to the same object.
//
// The update installer is started only after release(): with /SUPPRESSMSGBOXES,
// Inno's "the application is running" question answers Cancel and the silent update
// would end at once if this copy still held the mutex while it exits.

#include "wizz/services/updater.hpp"

#include <string>
#include <string_view>

namespace wizz::app {

/// The name in the installer's AppMutex directive (packaging/WizzairHelper.iss, D-36).
inline constexpr std::wstring_view kAppMutexName = update::kAppMutexName;

class AppMutex {
public:
    /// Creates the named mutex, or opens it when another copy already runs. A failure
    /// is not fatal (the app works without it): held() is then false.
    explicit AppMutex(std::wstring_view name = kAppMutexName);
    /// release().
    ~AppMutex();

    AppMutex(const AppMutex&) = delete;
    AppMutex& operator=(const AppMutex&) = delete;

    /// This process holds a handle to the mutex.
    [[nodiscard]] bool held() const noexcept { return handle_ != nullptr; }
    /// Closes this process's handle (idempotent); the mutex disappears with the last one.
    void release() noexcept;
    [[nodiscard]] const std::wstring& name() const noexcept { return name_; }

    /// Whether a mutex of that name exists in this session (what the installer asks).
    [[nodiscard]] static bool exists(std::wstring_view name);

private:
    std::wstring name_;
    void* handle_ = nullptr;  ///< HANDLE
};

} // namespace wizz::app
