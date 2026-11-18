#pragma once
// diagnostics.json: the test-only measurements of an --autotest run with the
// hidden `--diagnostics` companion (DECISIONS D-30), written next to summary.json
// (whose documented schema stays unchanged). It holds what 05 §5 asks to check
// beyond summary.json:
//
//   timer       the BusyPanel's 40 ms timer: runs, ticks, longest / mean gap and the
//               gaps above 50 ms (R-04) and 100 ms (05 §5); the most UI-thread CPU
//               time in one gap and the longest gap that other processes did not
//               hold the CPUs during (D-50) — TimerGapProbe;
//   uiEvents    the UI-loop watchdog (R-03): the longest uninterrupted UI-thread
//               dispatches, overall and while busy, with their CPU time and the
//               same attribution — EventWatchdog (null when the QApplication is
//               not wizz::app::Application);
//   cancel      --cancel-after-ms: when Esc and Cancel were pressed, what the panel
//               showed, and how many cancellation transitions they produced (D-11:
//               exactly one while busy);
//   searches    each search the script ran: Wizz requests it made (0 for a cached
//               replay), real duration, final status line;
//   grids       the final Outbound / Return tables against the stored results: rows,
//               result flights, rows that are not in the result (late batches or rows
//               of a previous search) and duplicate (From, To, Date) rows;
//   display     platform, QT_SCALE_FACTOR, screen, device pixel ratio, DPI, window
//               geometry in logical and device pixels, window on its screen;
//   layout      layout_problems() of the window (clipping, overlap, cut text) and
//               whether Cancel is fully visible;
//   environment OS, Qt version, locale, widget style, and the look in use (D-53..D-56):
//               light, dark or high_contrast, the window colour of its palette and
//               the family the UI font resolved to, so a run can tell that the look it
//               asked for (WIZZ_COLOR_SCHEME, the Windows app mode) is the one it got.
//
// Key order is fixed (as listed). UI thread only (the collectors read widgets).

#include "event_watchdog.hpp"
#include "timer_gap_probe.hpp"

#include "wizz/core/domain.hpp"
#include "wizz/core/locale.hpp"

#include <QRect>
#include <QSize>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class QWidget;

namespace wizz::app {

class MainWindow;

struct CancelDiagnostics {
    std::chrono::milliseconds after{0};  ///< --cancel-after-ms
    double pressed_at_ms = 0;            ///< real time from the leg's start to the presses
    bool busy_at_press = false;
    std::string status_before;           ///< the busy line just before the presses
    std::vector<std::string> presses;    ///< "Esc", "Cancel button", "Esc", "Cancel handler"
    int cancel_requests = 0;             ///< BusyPanel::cancel_requested emissions until the leg ended
    std::string status_after_press;      ///< the busy line right after the presses
    bool cancel_enabled_after_press = false;
};

struct SearchDiagnostics {
    std::string name;                    ///< "warm-up outbound", "outbound", "return"
    std::int64_t wizz_requests = 0;      ///< WizzApiClient::request_count() delta
    double duration_ms = 0;              ///< real time
    std::string final_status;            ///< the busy line when it ended
    bool final_status_cut = false;       ///< it ended with "…": too long for the status line
};

struct GridDiagnostics {
    std::string name;                         ///< "outbound" / "return"
    std::size_t rows = 0;                     ///< AllResults
    std::optional<std::size_t> result_flights;  ///< the stored result's flights (null: no result)
    std::size_t rows_not_in_result = 0;       ///< rows whose fare object is not in the result
    std::size_t duplicate_rows = 0;           ///< rows repeating an earlier row's (From, To, Date)
};

struct DisplayDiagnostics {
    std::string platform;                     ///< QGuiApplication::platformName()
    std::optional<std::string> scale_factor;  ///< QT_SCALE_FACTOR
    std::optional<std::string> screen_scale_factors;  ///< QT_SCREEN_SCALE_FACTORS
    std::string screen_name;
    QRect screen_geometry;                    ///< logical
    QRect screen_available;                   ///< logical work area
    double device_pixel_ratio = 1.0;          ///< the window's
    double logical_dpi = 96.0;
    double physical_dpi = 96.0;
    QRect window_frame;                       ///< logical, title bar included (Qt frame)
    QRect window_client;                      ///< logical
    QSize outer_device_size;                  ///< MainWindow::normal_outer_device_size()
    QSize minimum_window;                     ///< logical, whole window (MainWindow::kMinimumSize, 01 §4.1)
    QSize default_window;                     ///< logical, whole window (MainWindow::kDefaultSize, 01 §4.1)
    bool window_on_screen = false;            ///< the frame lies inside the work area
};

struct LayoutDiagnostics {
    int checks = 0;                           ///< window states checked (one per screenshot)
    std::vector<std::string> problems;        ///< "<state>: <widget>: <problem>" when merged
    bool cancel_visible = false;              ///< Cancel fully inside the client area in every state

    /// Adds the check of one window state named `state` (e.g. "1-outbound.png").
    void merge(std::string_view state, const LayoutDiagnostics& check);
};

/// One of the look's fonts as this machine draws it (D-58).
struct FontDiagnostics {
    std::string role;                         ///< "ui", "semibold", "caption", "mono" (the log), "monoSemibold" (elapsed time)
    std::string face;                         ///< the family of the face Qt picks for it
    int weight = 0;                           ///< that face's own weight (QFontInfo without the weight axis)
    std::optional<double> weight_axis;        ///< the "wght" value it draws with; none when it has none
};

struct EnvironmentDiagnostics {
    std::string os;                           ///< QSysInfo::prettyProductName()
    std::string kernel;                       ///< QSysInfo::kernelVersion()
    std::string qt;                           ///< qVersion()
    std::string locale;                       ///< the app's culture (07 §1)
    std::string style;                        ///< the widget style's name ("modern/fusion" for ModernStyle)
    std::string look;                         ///< "light", "dark" or "high_contrast" (style::appearance())
    std::string window_colour;                ///< the palette's Window colour, "#rrggbb"
    std::string font_family;                  ///< the family the UI font resolved to
    std::string text_rendering;               ///< the text rendering (style::text_rendering(), D-58)
    std::string font_hinting;                 ///< the UI font's hinting preference: "default", "none", "vertical", "full"
    std::optional<double> font_weight_axis;   ///< the UI font's "wght" axis value; none when it has none
    std::vector<FontDiagnostics> fonts;       ///< the UI, semibold, caption and monospace fonts, in that order
};

struct AutoTestDiagnostics {
    TimerGapProbe::Stats timer;
    std::optional<EventWatchdog::Stats> events;  ///< null without an Application (in-process tests)
    std::optional<CancelDiagnostics> cancel;
    std::vector<SearchDiagnostics> searches;
    std::vector<GridDiagnostics> grids;
    DisplayDiagnostics display;
    LayoutDiagnostics layout;
    EnvironmentDiagnostics environment;
};

/// The file name next to summary.json.
inline constexpr std::string_view kDiagnosticsFileName = "diagnostics.json";

/// The grid's rows against `result` (null = no stored result).
[[nodiscard]] GridDiagnostics describe_grid(std::string name, std::span<const FlightResultPtr> rows,
                                            const LegSearchResult* result);
/// The window's screen, scale and geometry.
[[nodiscard]] DisplayDiagnostics describe_display(const MainWindow& window);
/// layout_problems(window) and Cancel's visibility.
[[nodiscard]] LayoutDiagnostics describe_layout(const MainWindow& window);
[[nodiscard]] EnvironmentDiagnostics describe_environment(const Locale& locale);

/// The diagnostics.json document (two-space indentation).
[[nodiscard]] std::string to_json(const AutoTestDiagnostics& diagnostics);

} // namespace wizz::app
