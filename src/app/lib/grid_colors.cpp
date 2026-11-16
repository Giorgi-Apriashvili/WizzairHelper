#include "grid_colors.hpp"

#include "ui_style.hpp"

namespace wizz::app::grid_colors {

QColor gray_text()
{
    return style::theme().text_muted;
}

QColor highlight()
{
    return style::theme().selection;
}

QColor highlight_text()
{
    return style::theme().selection_text;
}

QColor cheapest_back()
{
    return style::theme().cheapest;
}

QColor cheapest_selection_back()
{
    return style::theme().cheapest_selection;
}

} // namespace wizz::app::grid_colors
