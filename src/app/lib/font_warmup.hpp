#pragma once
// FontWarmup: resolves, on a worker thread at start-up, the fonts and the
// Windows font fallback that texts shown only later need.
//
// The BusyPanel's retry countdown "⏳ retrying in {s} s" (01 §10.5) contains
// U+23F3, which Segoe UI lacks. The first time Qt lays out that glyph it asks
// Windows for a fallback font and loads it: 0.2–0.4 s on the UI thread (measured
// on the "windows" platform, Release). Without the warm-up that happens when the
// first retry notice arrives, so the spinner and the 40 ms timer freeze exactly
// while the user watches a back-off (05 §5 / R-03: longest timer gap 400 ms in
// the cancel_retrying acceptance run). The font database and its fallback cache
// are shared by all threads, so a worker can pay that cost while the UI thread
// builds the services and the window; main() waits for it before the window is
// shown, so no busy period ever meets the cold path.
//
// Thread-safety: QFont and QFontMetrics are reentrant and QFontDatabase is
// thread-safe; the worker only measures text with its own QFont copies. The
// QApplication must outlive the warm-up (wait() or the destructor joins).

#include <QFont>
#include <QString>

#include <thread>
#include <vector>

namespace wizz::app {

class FontWarmup {
public:
    /// The fonts the app draws text in (ui_style.hpp): the UI font first, then the semibold
    /// one (also the busy panel's wait note), the data, monospace, hint and caption fonts.
    [[nodiscard]] static std::vector<QFont> default_fonts();
    /// Every character the progress UI can show that Segoe UI may lack, in one sample.
    [[nodiscard]] static QString default_sample();

    /// Starts measuring `sample` in each of `fonts` on a worker thread.
    explicit FontWarmup(std::vector<QFont> fonts = default_fonts(), QString sample = default_sample());
    /// Waits for the worker.
    ~FontWarmup();

    FontWarmup(const FontWarmup&) = delete;
    FontWarmup& operator=(const FontWarmup&) = delete;

    /// Blocks until the worker is done (call before the first window is shown); later
    /// calls return at once.
    void wait();

private:
    std::jthread thread_;
};

} // namespace wizz::app
