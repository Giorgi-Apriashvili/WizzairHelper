#pragma once
// The named mutex the installer looks for (DECISIONS D-36). WizzairHelper.exe creates
// "WizzairHelper.AppMutex" at start-up and holds it for the life of the process, so Inno
// Setup (AppMutex= in packaging/WizzairHelper.iss) can tell that the app is running before
// it replaces or removes its files. It is not a single-instance lock: every copy creates or
// opens the same mutex and keeps running (06 §3 allows concurrent instances), and the object
// disappears when the last copy ends.

#include <string>
#include <string_view>

namespace wizz::app {

/// The name in the installer's AppMutex directive (packaging/WizzairHelper.iss).
inline constexpr std::wstring_view kAppMutexName = L"WizzairHelper.AppMutex";

class AppMutex final {
public:
    /// Creates the named mutex, or opens it when another copy already holds it. A mutex
    /// Windows refuses (e.g. another kind of object with that name) is not an error: the
    /// app runs as usual and the installer just cannot see it.
    explicit AppMutex(std::wstring_view name = kAppMutexName);
    ~AppMutex();

    AppMutex(const AppMutex&) = delete;
    AppMutex& operator=(const AppMutex&) = delete;
    AppMutex(AppMutex&&) = delete;
    AppMutex& operator=(AppMutex&&) = delete;

    /// Whether this process holds a handle to the mutex.
    [[nodiscard]] bool held() const noexcept { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;  ///< HANDLE of the mutex; null when Windows refused it
};

} // namespace wizz::app
