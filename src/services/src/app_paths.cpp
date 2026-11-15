#include "wizz/services/app_paths.hpp"

#include <windows.h>
#include <shlobj.h>

#include <memory>
#include <system_error>

namespace wizz::app_paths {
namespace {

struct CoTaskMemFreer {
    void operator()(wchar_t* memory) const noexcept { ::CoTaskMemFree(memory); }
};

[[nodiscard]] std::filesystem::path local_app_data()
{
    PWSTR raw = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    const std::unique_ptr<wchar_t, CoTaskMemFreer> owner{raw}; // must be freed even when the call fails
    if (FAILED(result)) {
        throw std::system_error(static_cast<int>(result), std::system_category(),
                                "SHGetKnownFolderPath(FOLDERID_LocalAppData) failed");
    }
    return std::filesystem::path{owner.get()};
}

} // namespace

std::filesystem::path default_root(const std::optional<std::filesystem::path>& root_override)
{
    return root_override ? *root_override : local_app_data() / L"WizzairHelper";
}

std::filesystem::path default_cache_dir(const std::optional<std::filesystem::path>& root_override)
{
    return default_root(root_override) / L"cache";
}

std::filesystem::path settings_file(const std::optional<std::filesystem::path>& root_override)
{
    return default_root(root_override) / L"settings.json";
}

std::filesystem::path errors_log(const std::optional<std::filesystem::path>& root_override)
{
    return default_root(root_override) / L"errors.log";
}

} // namespace wizz::app_paths
