#include "app_info.hpp"

#include <QSize>

// The icon's PNGs are the Qt resource "app_icon" of the static library wizzairhelper_app
// (src/app/CMakeLists.txt). Qt's CMake links an object that registers it into each executable
// linking the library through CMake; registering it here too works however the library is
// linked, and a second registration is a no-op. Q_INIT_RESOURCE declares the registration
// function, so it must be used outside any namespace.
static void register_app_icon_resource()
{
    Q_INIT_RESOURCE(app_icon);
}

namespace wizz::app {

QString product_name()
{
    return QStringLiteral("WizzairHelper");
}

QString main_window_title()
{
    return QStringLiteral("WizzairHelper — cheapest Wizz Air flights from nearby airports");
}

QIcon application_icon()
{
    register_app_icon_resource();
    QIcon icon;
    for (const int size : kAppIconSizes) {
        icon.addFile(QStringLiteral(":/icons/WizzairHelper-%1.png").arg(size), QSize(size, size));
    }
    return icon;
}

} // namespace wizz::app
