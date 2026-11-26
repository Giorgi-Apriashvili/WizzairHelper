#pragma once
// The look the user picks with the Settings window's Theme switch (DECISIONS D-75),
// settings.json's `Theme`: Auto follows the Windows app mode (the behaviour before
// 2.6.0); Dark and Light keep that look whatever Windows uses. Windows High Contrast and
// the WIZZ_COLOR_SCHEME diagnostic switch still win over it (ui_style.hpp in the app).

#include <array>

namespace wizz::ui {

enum class ThemeChoice { automatic, dark, light };

/// The switch's choices, left to right ("Auto | Dark | Light", the owner's order).
inline constexpr std::array<ThemeChoice, 3> kThemeChoices{ThemeChoice::automatic, ThemeChoice::dark,
                                                          ThemeChoice::light};

} // namespace wizz::ui
