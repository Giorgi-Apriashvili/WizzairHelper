#pragma once
// The client area of the smallest main window (01 §4.1: L(980) x L(600) is the whole
// window, frame included) at 100 %: what the cards and the results must fit in. The
// tests' "minimal" platform has no native frame, so its windows are all client area.

#include "main_window.hpp"

#include <QSize>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wizz::test::gui {

/// MainWindow::kMinimumSize minus the frame Windows gives an overlapped window at 96 DPI
/// (AdjustWindowRectExForDpi: 8 px borders and a 31 px title bar on Windows 10 and 11).
[[nodiscard]] inline QSize minimum_client_size()
{
    RECT frame{0, 0, 0, 0};
    if (AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, USER_DEFAULT_SCREEN_DPI) == FALSE) {
        return app::MainWindow::kMinimumSize;
    }
    return app::MainWindow::kMinimumSize - QSize(frame.right - frame.left, frame.bottom - frame.top);
}

} // namespace wizz::test::gui
