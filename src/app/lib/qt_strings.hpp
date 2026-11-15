#pragma once
// UTF-8 std::string (core, services, ui_logic) <-> QString (widgets). The
// conversion happens only at the widget boundary (ARCHITECTURE.md).

#include <QString>

#include <string>
#include <string_view>

namespace wizz::app {

[[nodiscard]] inline QString to_qstring(std::string_view utf8)
{
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

[[nodiscard]] inline std::string to_utf8(const QString& text)
{
    return text.toStdString();
}

} // namespace wizz::app
