#pragma once
// The user's update options (Settings window, settings.json; DECISIONS D-59..D-61):
// whether and how often the app checks GitHub on its own, and what an installed copy
// does with an update it found. Plain values, shared by the schedule (updater.hpp),
// settings.json (wizz/ui/settings.hpp) and the update rules (wizz/ui/update_model.hpp).
//
// The defaults are the behaviour of 2.0 and 2.1: check once a day, download the
// installer and install it when the app closes.

#include <cstdint>

namespace wizz::update {

/// How often the automatic check runs: once per local calendar day, ISO 8601 week
/// (Monday to Sunday) or month (settings.json `UpdateCheckFrequency`).
enum class CheckFrequency : std::uint8_t {
    daily,    ///< "Daily": "Once a day"
    weekly,   ///< "Weekly": "Once a week"
    monthly,  ///< "Monthly": "Once a month"
};

/// What an installed copy does when a check finds a newer release (settings.json
/// `WhenUpdateFound`). A portable copy cannot install itself and ignores it.
enum class WhenFound : std::uint8_t {
    install_on_close,   ///< "InstallOnClose": download and verify, install when the app closes (2.0 / 2.1)
    download_then_ask,  ///< "DownloadThenAsk": download and verify, install only on "Restart and update"
    notify_only,        ///< "NotifyOnly": no download until the user asks ("Update now")
};

struct UpdateOptions {
    /// settings.json `CheckForUpdates`: false = no request to GitHub unless the user
    /// presses Check now (or an update action in the update bar).
    bool check_automatically = true;
    CheckFrequency check_frequency = CheckFrequency::daily;
    WhenFound when_found = WhenFound::install_on_close;

    friend constexpr bool operator==(const UpdateOptions&, const UpdateOptions&) noexcept = default;
};

} // namespace wizz::update
