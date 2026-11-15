#include "app_info.hpp"

namespace wizz::app {

QString product_name()
{
    return QStringLiteral("WizzairHelper");
}

QString main_window_title()
{
    return QStringLiteral("WizzairHelper — cheapest Wizz Air flights from nearby airports");
}

} // namespace wizz::app
