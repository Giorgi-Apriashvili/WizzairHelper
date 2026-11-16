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
// The worker also DRAWS each font (warm()), because measuring reads only the
// metrics: the first drawing still reads the glyph outlines and the colour layers
// of the fallback font. Measured in fresh processes (DECISIONS D-33): after a
// measure-only warm-up the UI thread's first drawing of the retry note took about
// 840 page faults, after a drawing warm-up 4 to 34. With a warm file cache those
// faults cost 3 ms; on a cold one (GitHub's runner) they were disk reads and froze
// the first retry note for 149 ms.
//
// Thread-safety: QFont and QFontMetrics are reentrant, QFontDatabase is
// thread-safe, and painting on a QImage is allowed outside the GUI thread; the
// worker uses its own QFont copies and images. The QApplication must outlive the
// warm-up (wait() or the destructor joins).

#include <QFont>
#include <QImage>
#include <QString>

#include <functional>
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

    /// What the worker does with each font: lays `sample` out and draws it into an image,
    /// which it returns (for tests; the worker drops it).
    [[nodiscard]] static QImage warm(const QFont& font, const QString& sample);
    /// The per-font step the worker runs; empty means warm().
    using Step = std::function<void(const QFont&, const QString&)>;

    /// Starts warming `sample` in each of `fonts`, in order, on a worker thread.
    explicit FontWarmup(std::vector<QFont> fonts = default_fonts(), QString sample = default_sample(),
                        Step step = {});
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
