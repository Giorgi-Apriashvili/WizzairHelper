#pragma once
// The client areas of the minimum and the default main window (01 §4.1: L(980) x L(600)
// and L(1320) x L(820) are whole windows, frame included) at 100 %: what the cards and
// the results must fit in. The tests' "minimal" platform has no native frame, so its
// windows are all client area.

#include "main_window.hpp"

#include <QSize>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wizz::test::gui {

/// `outer` (a whole window, 01 §4.1) minus the frame Windows gives an overlapped window at
/// 96 DPI (AdjustWindowRectExForDpi: 8 px borders and a 31 px title bar on Windows 10 and 11).
[[nodiscard]] inline QSize client_size(QSize outer)
{
    RECT frame{0, 0, 0, 0};
    if (AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, USER_DEFAULT_SCREEN_DPI) == FALSE) {
        return outer;
    }
    return outer - QSize(frame.right - frame.left, frame.bottom - frame.top);
}

/// The client area of the 980 x 600 minimum window (964 x 561).
[[nodiscard]] inline QSize minimum_client_size()
{
    return client_size(::wizz::app::MainWindow::kMinimumSize);
}

/// The client area of the default 1320 x 820 window (1304 x 781).
[[nodiscard]] inline QSize default_client_size()
{
    return client_size(::wizz::app::MainWindow::kDefaultSize);
}

} // namespace wizz::test::gui
