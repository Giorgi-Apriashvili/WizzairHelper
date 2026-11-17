#pragma once
// The look the GUI tests run under (DECISIONS D-53..D-56): the app's own, as
// WizzairHelper.exe installs it at start-up (style::apply_application_look():
// ModernStyle over Fusion, the theme's palette and tooltip palette, the UI
// font), with the light tokens and High Contrast off, so a test sees what a
// user on a light Windows sees, whatever the machine's settings are.
// WIZZ_COLOR_SCHEME=dark when the binary starts runs the whole suite in the
// dark look instead (tests compare with style::theme(), not with fixed colours).
//
// WIZZ_TEXT_RENDERING=natural|hinted runs it with that text rendering (D-58).
//
// LookGuard is for the tests that change the look (scheme, High Contrast probe,
// Windows 10 fonts, text rendering): it clears the switches for the test (the
// default rendering, natural-heavier) and puts the suite's look back afterwards,
// also after a failed ASSERT.

#include "ui_style.hpp"

#include <QByteArray>
#include <QtGlobal>

#include <array>
#include <optional>
#include <utility>

namespace wizz::test::look {

/// The environment switches of the look (ui_style.hpp).
inline constexpr std::array<const char*, 4> kSwitches{"WIZZ_COLOR_SCHEME", "WIZZ_HIGH_CONTRAST", "WIZZ_FONTS",
                                                      "WIZZ_TEXT_RENDERING"};

/// Installs the suite's look: light unless WIZZ_COLOR_SCHEME says otherwise, High
/// Contrast off. Called by the test main once the QApplication exists.
inline void apply_suite_look()
{
    if (!qEnvironmentVariableIsSet("WIZZ_COLOR_SCHEME")) {
        qputenv("WIZZ_COLOR_SCHEME", "light");
    }
    static_cast<void>(::wizz::app::style::set_high_contrast_probe([] { return false; }));
    static_cast<void>(::wizz::app::style::apply_application_look());
}

class LookGuard {
public:
    LookGuard()
    {
        for (std::size_t i = 0; i < kSwitches.size(); ++i) {
            if (qEnvironmentVariableIsSet(kSwitches[i])) {
                saved_[i] = qgetenv(kSwitches[i]);
            }
            qunsetenv(kSwitches[i]);
        }
        previous_probe_ = ::wizz::app::style::set_high_contrast_probe([] { return false; });
    }
    ~LookGuard()
    {
        for (std::size_t i = 0; i < kSwitches.size(); ++i) {
            if (saved_[i]) {
                qputenv(kSwitches[i], *saved_[i]);
            } else {
                qunsetenv(kSwitches[i]);
            }
        }
        ::wizz::app::style::set_high_contrast_probe(std::move(previous_probe_));
        static_cast<void>(::wizz::app::style::apply_application_look());
    }
    LookGuard(const LookGuard&) = delete;
    LookGuard& operator=(const LookGuard&) = delete;

private:
    std::array<std::optional<QByteArray>, kSwitches.size()> saved_;
    ::wizz::app::style::HighContrastProbe previous_probe_;
};

} // namespace wizz::test::look
