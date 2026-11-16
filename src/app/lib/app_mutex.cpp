#include "app_mutex.hpp"

#include <windows.h>

namespace wizz::app {

AppMutex::AppMutex(std::wstring_view name)
{
    // Not owned (bInitialOwner FALSE): the installer only asks whether the object exists,
    // and every copy of the app holds a handle of its own. CreateMutexW opens an existing
    // mutex of that name (ERROR_ALREADY_EXISTS), which is how a second copy shares it.
    const std::wstring terminated(name);
    handle_ = CreateMutexW(nullptr, FALSE, terminated.c_str());
}

AppMutex::~AppMutex()
{
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

} // namespace wizz::app
