#include "window_capture.hpp"

#include <QGuiApplication>
#include <QString>
#include <QWidget>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dwmapi.h>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace wizz::app {

namespace {

struct DcReleaser {
    void operator()(HDC dc) const noexcept { ReleaseDC(nullptr, dc); }
};
struct DcDeleter {
    void operator()(HDC dc) const noexcept { DeleteDC(dc); }
};
struct ObjectDeleter {
    void operator()(HBITMAP bitmap) const noexcept { DeleteObject(bitmap); }
};
using ScreenDc = std::unique_ptr<std::remove_pointer_t<HDC>, DcReleaser>;
using MemoryDc = std::unique_ptr<std::remove_pointer_t<HDC>, DcDeleter>;
using Bitmap = std::unique_ptr<std::remove_pointer_t<HBITMAP>, ObjectDeleter>;

/// Restores the DC's previous bitmap before the DIB is deleted.
class SelectGuard {
public:
    SelectGuard(HDC dc, HGDIOBJ object) noexcept : dc_(dc), previous_(SelectObject(dc, object)) {}
    SelectGuard(const SelectGuard&) = delete;
    SelectGuard& operator=(const SelectGuard&) = delete;
    ~SelectGuard() { SelectObject(dc_, previous_); }

private:
    HDC dc_;
    HGDIOBJ previous_;
};

[[nodiscard]] std::string last_error_text(const char* step)
{
    return std::string{step} + " failed (Win32 error " + std::to_string(GetLastError()) + ")";
}

} // namespace

std::expected<QImage, std::string> capture_window(const QWidget& widget)
{
    const QWidget* window = widget.window();
    if (QGuiApplication::platformName() != QStringLiteral("windows") || window->windowHandle() == nullptr) {
        return std::unexpected{std::string{"the window has no native handle on the \""} +
                               QGuiApplication::platformName().toStdString() + "\" platform"};
    }
    const auto hwnd = reinterpret_cast<HWND>(window->winId());
    // PW_RENDERFULLCONTENT reads the window's DWM surface. Qt's last paint reached the
    // window through GDI, so flush the GDI batch and wait for the next composition;
    // otherwise the capture can mix the previous frame with the new one.
    GdiFlush();
    if (FAILED(DwmFlush())) {
        // Composition is always on since Windows 8; without it there is nothing to wait for.
    }
    RECT bounds{};
    if (!GetWindowRect(hwnd, &bounds)) {
        return std::unexpected{last_error_text("GetWindowRect")};
    }
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        return std::unexpected{std::string{"the window has no area"}};
    }

    const ScreenDc screen{GetDC(nullptr)};
    if (!screen) {
        return std::unexpected{last_error_text("GetDC")};
    }
    const MemoryDc memory{CreateCompatibleDC(screen.get())};
    if (!memory) {
        return std::unexpected{last_error_text("CreateCompatibleDC")};
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down rows, like QImage
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const Bitmap dib{CreateDIBSection(memory.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0)};
    if (!dib || bits == nullptr) {
        return std::unexpected{last_error_text("CreateDIBSection")};
    }
    QImage image;
    {
        const SelectGuard select{memory.get(), dib.get()};
        if (!PrintWindow(hwnd, memory.get(), PW_RENDERFULLCONTENT)) {
            return std::unexpected{last_error_text("PrintWindow")};
        }
        GdiFlush();
        // BGRX rows; the X byte is undefined, so read it as opaque RGB32 and copy out of the DIB.
        image = QImage{static_cast<const uchar*>(bits), width, height, width * 4, QImage::Format_RGB32}.copy();
    }

    // Crop the invisible resize borders (the DWM frame is inside the window rectangle).
    RECT frame{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        const QRect visible{frame.left - bounds.left, frame.top - bounds.top, frame.right - frame.left,
                            frame.bottom - frame.top};
        const QRect clipped = visible.intersected(image.rect());
        if (!clipped.isEmpty() && clipped != image.rect()) {
            image = image.copy(clipped);
        }
    }
    return image;
}

std::expected<void, std::string> save_window_png(const QWidget& widget, const std::filesystem::path& path)
{
    std::expected<QImage, std::string> image = capture_window(widget);
    if (!image) {
        return std::unexpected{std::move(image.error())};
    }
    if (!image->save(QString::fromStdWString(path.wstring()), "PNG")) {
        return std::unexpected{std::string{"the PNG file could not be written"}};
    }
    return {};
}

} // namespace wizz::app
