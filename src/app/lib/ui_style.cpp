#include "ui_style.hpp"

#include <QApplication>
#include <QStyle>
#include <QStyleFactory>

#include <windows.h>

namespace wizz::app::style {

QFont form_font()
{
    QFont font(QStringLiteral("Segoe UI"));
    font.setPointSizeF(9.0);
    return font;
}

QFont bold_form_font()
{
    QFont font = form_font();
    font.setBold(true);
    return font;
}

QColor wizz_magenta()
{
    return QColor(197, 0, 110);
}

QColor gray_text()
{
    const DWORD rgb = ::GetSysColor(COLOR_GRAYTEXT);
    return QColor(GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
}

bool apply_application_look()
{
    QApplication::setFont(form_font());
    QStyle* native = QStyleFactory::create(QStringLiteral("windowsvista"));
    if (native == nullptr) {
        return false;
    }
    QApplication::setStyle(native);  // the application takes ownership
    return true;
}

QColor search_button_disabled_text()
{
    // 60 % white over RGB(197, 0, 110).
    return QColor(232, 153, 197);
}

} // namespace wizz::app::style
