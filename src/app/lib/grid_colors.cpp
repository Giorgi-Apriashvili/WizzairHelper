#include "grid_colors.hpp"

#include <windows.h>

namespace wizz::app::grid_colors {

namespace {

[[nodiscard]] QColor system_color(int index)
{
    const DWORD rgb = GetSysColor(index);
    return QColor{GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)};
}

} // namespace

QColor gray_text()
{
    return system_color(COLOR_GRAYTEXT);
}

QColor highlight()
{
    return system_color(COLOR_HIGHLIGHT);
}

QColor highlight_text()
{
    return system_color(COLOR_HIGHLIGHTTEXT);
}

} // namespace wizz::app::grid_colors
