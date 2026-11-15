#include "system_colours.hpp"

#include <windows.h>

namespace wizz::app {

QColor system_colour(SystemColour colour)
{
    int index = COLOR_3DLIGHT;
    switch (colour) {
    case SystemColour::control_light:
        index = COLOR_3DLIGHT;
        break;
    case SystemColour::control_text:
        index = COLOR_BTNTEXT;
        break;
    case SystemColour::window:
        index = COLOR_WINDOW;
        break;
    case SystemColour::window_text:
        index = COLOR_WINDOWTEXT;
        break;
    }
    const COLORREF value = GetSysColor(index);
    return {GetRValue(value), GetGValue(value), GetBValue(value)};
}

} // namespace wizz::app
