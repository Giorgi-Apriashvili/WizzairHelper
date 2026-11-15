#pragma once
// The WinForms SystemColors the spec names (01 §10.1 ControlLight / ControlText,
// §12.1 Window), read from the Windows colour scheme (GetSysColor) like .NET
// does. Qt styles repaint their palettes (the "windows11" style turns Midlight
// white and text-box backgrounds grey), so widgets that the spec pins to a system
// colour take it from here and re-read it on palette and theme changes.

#include <QColor>

namespace wizz::app {

enum class SystemColour {
    control_light,  ///< COLOR_3DLIGHT: the busy panel background
    control_text,   ///< COLOR_BTNTEXT: label text
    window,         ///< COLOR_WINDOW: text box background
    window_text,    ///< COLOR_WINDOWTEXT: text box text
};

[[nodiscard]] QColor system_colour(SystemColour colour);

} // namespace wizz::app
