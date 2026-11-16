// Updater: SHA-256 through Windows CNG, the update log, install-kind detection, the
// silent installer command line and stale-download cleanup (D-43, D-44).

#include "wizz/services/updater.hpp"

#include "updater_detail.hpp"
#include "win32_util.hpp"

#include "wizz/core/datetime_parse.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace wizz::update {
namespace {

constexpr DWORD kReadChunk = 1u << 20;

struct AlgorithmCloser {
    void operator()(BCRYPT_ALG_HANDLE handle) const noexcept { ::BCryptCloseAlgorithmProvider(handle, 0); }
};
struct HashDestroyer {
    void operator()(BCRYPT_HASH_HANDLE handle) const noexcept { ::BCryptDestroyHash(handle); }
};
using UniqueAlgorithm = std::unique_ptr<void, AlgorithmCloser>;
using UniqueHash = std::unique_ptr<void, HashDestroyer>;

void check_status(NTSTATUS status, std::string_view call)
{
    if (!BCRYPT_SUCCESS(status)) {
        throw std::runtime_error(std::format("{} failed with NTSTATUS {:#010x}", call, static_cast<unsigned long>(status)));
    }
}

/// One SHA-256 computation (CNG provider and hash object owned for its lifetime).
class Sha256 {
public:
    Sha256()
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        check_status(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0),
                     "BCryptOpenAlgorithmProvider");
        algorithm_.reset(algorithm);
        BCRYPT_HASH_HANDLE hash = nullptr;
        // No hash-object buffer: CNG allocates and frees it with the handle (Windows 7+).
        check_status(::BCryptCreateHash(algorithm_.get(), &hash, nullptr, 0, nullptr, 0, 0), "BCryptCreateHash");
        hash_.reset(hash);
    }

    void update(std::string_view bytes)
    {
        while (!bytes.empty()) {
            const auto chunk = static_cast<ULONG>(std::min<std::size_t>(bytes.size(), kReadChunk));
            // BCryptHashData takes a non-const pointer but only reads through it.
            check_status(::BCryptHashData(hash_.get(), reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), chunk, 0),
                         "BCryptHashData");
            bytes.remove_prefix(chunk);
        }
    }

    [[nodiscard]] std::string finish_hex()
    {
        std::array<UCHAR, 32> digest{};
        check_status(::BCryptFinishHash(hash_.get(), digest.data(), static_cast<ULONG>(digest.size()), 0),
                     "BCryptFinishHash");
        std::string hex;
        hex.reserve(2 * digest.size());
        for (const UCHAR byte : digest) {
            hex += std::format("{:02x}", byte);
        }
        return hex;
    }

private:
    UniqueAlgorithm algorithm_;
    UniqueHash hash_;
};

/// `WizzairHelper-<X.Y.Z>-win64-setup.exe` or `-win64.zip`, optionally with `.tmp`:
/// the version in the name, nullopt for any other name.
[[nodiscard]] std::optional<Version> download_version(std::string_view name)
{
    if (name.ends_with(".tmp")) {
        name.remove_suffix(4);
    }
    if (!name.starts_with(kAssetPrefix)) {
        return std::nullopt;
    }
    name.remove_prefix(kAssetPrefix.size());
    for (const std::string_view suffix : {kInstallerSuffix, kZipSuffix}) {
        if (name.ends_with(suffix)) {
            return parse_version(name.substr(0, name.size() - suffix.size()));
        }
    }
    return std::nullopt;
}

} // namespace

// --- SHA-256 -------------------------------------------------------------------------

std::string sha256_hex(std::string_view bytes)
{
    Sha256 hash;
    hash.update(bytes);
    return hash.finish_hex();
}

std::optional<std::string> sha256_file(const std::filesystem::path& path)
{
    const HANDLE raw = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    const win32::UniqueHandle file{raw};
    Sha256 hash;
    std::string buffer(kReadChunk, '\0');
    for (;;) {
        DWORD read = 0;
        if (::ReadFile(raw, buffer.data(), kReadChunk, &read, nullptr) == FALSE) {
            return std::nullopt;
        }
        if (read == 0) {
            return hash.finish_hex();
        }
        hash.update(std::string_view{buffer.data(), read});
    }
}

// --- The update log ------------------------------------------------------------------

namespace detail {

void append_raw_line(const std::filesystem::path& log_file, std::string_view line)
{
    std::string text{line};
    std::ranges::replace(text, '\r', ' ');
    std::ranges::replace(text, '\n', ' ');
    text += "\r\n";

    std::error_code ignored;
    std::filesystem::create_directories(log_file.parent_path(), ignored);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (::GetFileAttributesExW(log_file.c_str(), GetFileExInfoStandard, &attributes) != FALSE &&
        ((std::uintmax_t{attributes.nFileSizeHigh} << 32) | attributes.nFileSizeLow) > kLogRotateBytes) {
        std::filesystem::path old = log_file;
        old += L".old";
        ::MoveFileExW(log_file.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING); // a failure keeps appending
    }
    // FILE_APPEND_DATA makes each WriteFile land at the end, also with another
    // instance appending at the same time.
    const HANDLE raw = ::CreateFileW(log_file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return;
    }
    const win32::UniqueHandle file{raw};
    DWORD written = 0;
    ::WriteFile(raw, text.data(), static_cast<DWORD>(text.size()), &written, nullptr); // errors are ignored
}

} // namespace detail

void append_log_line(const std::filesystem::path& log_file, UtcInstant now, std::string_view text)
{
    detail::append_raw_line(log_file, std::format("{} {}", format_iso_utc(now), text));
}

// --- Installing ----------------------------------------------------------------------

InstallKind detect_install_kind(const std::filesystem::path& exe_path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(exe_path.parent_path() / kUninstallerName, error) ? InstallKind::installed
                                                                                               : InstallKind::portable;
}

std::filesystem::path running_executable_path()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "GetModuleFileNameW failed");
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path{std::move(buffer)};
        }
        buffer.resize(buffer.size() * 2); // truncated: try a larger buffer
    }
}

std::string InstallerCommand::parameters() const
{
    std::string joined;
    for (const auto& argument : arguments) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += argument;
    }
    return joined;
}

std::wstring InstallerCommand::command_line() const
{
    // A Windows path cannot contain '"', so quoting it whole is exact for CommandLineToArgvW.
    std::wstring line = L"\"" + program.wstring() + L"\"";
    if (const std::string switches = parameters(); !switches.empty()) {
        line += L' ';
        line += win32::to_wide(switches);
    }
    return line;
}

InstallerCommand silent_install_command(const std::filesystem::path& setup_exe, Relaunch relaunch)
{
    InstallerCommand command{.program = setup_exe,
                             .arguments = {"/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CLOSEAPPLICATIONS"}};
    if (relaunch == Relaunch::yes) {
        command.arguments.emplace_back("/RELAUNCH");
    }
    return command;
}

std::size_t remove_stale_downloads(const UpdateConfig& config)
{
    std::vector<std::filesystem::path> stale;
    std::error_code error;
    for (std::filesystem::directory_iterator it{config.download_dir(), error}, end; !error && it != end;
         it.increment(error)) {
        std::error_code type_error;
        if (!it->is_regular_file(type_error)) {
            continue;
        }
        const auto version = download_version(win32::to_utf8(it->path().filename().wstring()));
        if (version && *version <= config.current_version) {
            stale.push_back(it->path());
        }
    }
    std::size_t removed = 0;
    for (const auto& path : stale) {
        std::error_code remove_error;
        if (std::filesystem::remove(path, remove_error)) {
            ++removed;
        }
    }
    return removed;
}

} // namespace wizz::update
