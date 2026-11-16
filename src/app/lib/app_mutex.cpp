#include "app_mutex.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wizz::app {

AppMutex::AppMutex(std::wstring_view name) : name_(name)
{
    // Not owned: the installer only asks whether a mutex of this name exists. When
    // another copy runs, CreateMutexW opens the same object (ERROR_ALREADY_EXISTS).
    handle_ = ::CreateMutexW(nullptr, FALSE, name_.c_str());
}

AppMutex::~AppMutex()
{
    release();
}

void AppMutex::release() noexcept
{
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
}

bool AppMutex::exists(std::wstring_view name)
{
    const std::wstring terminated{name};
    const HANDLE handle = ::OpenMutexW(SYNCHRONIZE, FALSE, terminated.c_str());
    if (handle == nullptr) {
        return ::GetLastError() == ERROR_ACCESS_DENIED;  // there, but not ours to open
    }
    ::CloseHandle(handle);
    return true;
}

} // namespace wizz::app
