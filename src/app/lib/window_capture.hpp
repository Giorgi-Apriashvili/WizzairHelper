#pragma once
// Whole-window screenshots for --autotest (01 §17.2 `Screenshot(name)`):
// PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT) into a 32-bit DIB of the window
// rectangle, title bar and borders included, then cropped to the visible frame
// (DWMWA_EXTENDED_FRAME_BOUNDS drops the invisible resize borders of Windows
// 10/11). The image is in device pixels.

#include <QImage>

#include <expected>
#include <filesystem>
#include <string>

class QWidget;

namespace wizz::app {

/// Captures the top-level window of `widget`. The error text names the failing step
/// (no native window under the "minimal" / "offscreen" platforms, PrintWindow failure).
[[nodiscard]] std::expected<QImage, std::string> capture_window(const QWidget& widget);

/// Captures `widget`'s window and writes it as PNG to `path`.
[[nodiscard]] std::expected<void, std::string> save_window_png(const QWidget& widget, const std::filesystem::path& path);

} // namespace wizz::app
