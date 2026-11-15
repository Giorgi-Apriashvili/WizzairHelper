# WizzairHelper — desktop application design (Qt 6 Widgets)

How the 01 desktop experience maps onto Qt. Behaviour is defined by 01 (and 06 §2, 07); the pure
rules live in `wizzairhelper_ui_logic` (Qt-free, unit-tested). Widgets in `src/app` are thin: they
render ui_logic view state, forward user input, and never contain spec rules that can be tested
without a window.

## Process and shell (01 §3, §15, §17)

- `main.cpp`: install the crash handler; parse `--autotest` options (ui_logic, from the UTF-16
  command line so `--to ""` and non-ASCII places survive) *before* `QApplication` — a bad date or
  `--fixed-now` goes to the crash box before any window; point the reporter at `--data-root`;
  `PassThrough` rounding (per-monitor DPI v2 is Qt 6's default); `Application`;
  `style::apply_application_look()` (windowsvista + Segoe UI 9 pt); `FontWarmup` (a worker resolves
  the busy panel's "⏳" font fallback, 0.2–0.4 s the first time, D-33); `AppServices::create`
  (`ServiceConfig::for_run`: CurlTransport / ReplayTransport, SystemClock / FixedClock, user
  locale, data-root-aware `FileCache`s, `--no-pacing`); `AppSettings::load`; the window is built;
  `FontWarmup::wait()`; `MainWindow::show_main_window()`; `exec()`.
- `AppServices` (`app_services.hpp`): one transport shared by `WizzApiClient`, `CurrencyService`
  and `PlaceResolver`, plus `FlightSearchService`, the clock, the locale and the data root.
- `CrashReporter` (`crash_reporter.hpp/.cpp`): `show_crash(message, details)` appends
  `"[" + local yyyy-MM-ddTHH:mm:ss + "] " + details + CRLF + CRLF` to `errors.log` (ignore I/O
  errors) and shows the exact MessageBox (caption `WizzairHelper`, Error icon, literal
  `%LOCALAPPDATA%` text). `details` = exception type + message + `std::stacktrace`. Hooks:
  try/catch around posted tasks and event dispatch (`QApplication::notify` override),
  `SetUnhandledExceptionFilter`, `std::set_terminate`; worker-thread exceptions are posted to the UI.
  - `Application` (`application.hpp`) is the `QApplication` subclass whose `notify()` runs every
    event through `CrashReporter::run_guarded`; the app keeps running after the box.
  - Workers: `catch (...) { CrashReporter::post_current_exception(); }` at the top of the thread
    function (describes the exception there, shows the box on the UI thread).
  - MSVC x64 runs a catch block only after the throwing frames are unwound, so `install()` also
    adds a vectored exception handler that records the stack of each C++ throw per thread; the
    log then starts at the throw site. MSVC keeps terminate handlers per thread
    (`CrashReporter::install_terminate_handler()` for worker threads).
  - The dialog is injectable (tests); `set_data_root()` applies `--data-root` after parsing.
- Logical pixels: Qt widget coordinates are already DPI-independent, so 01's `L(x)` is just `x`.
  Settings `WindowWidth/Height` are *device* pixels: multiply/divide by `devicePixelRatio()`.
- Window size (01 §4.1, §13): a WinForms `Size` / `Bounds` is the whole window rectangle, the
  invisible resize borders of Windows 10/11 included, while Qt sizes the client area and reports
  only the visible frame (the title bar). `MainWindow::apply_start_geometry` therefore computes the
  full frame with `AdjustWindowRectExForDpi` (`window_placement.hpp`): `GetWindowRect` is exactly
  L(1320) × L(820) (or the saved size, clamped to [L(980) × L(600), work area of the cursor's
  screen]), centred in that work area; the minimum is the whole window too. Closing saves
  `GetWindowPlacement().rcNormalPosition` (RestoreBounds), so the size round-trips.

## Widgets (one class per file in `src/app`)

| Class | Spec | Responsibility |
|---|---|---|
| `MainWindow` (QMainWindow) | 01 §4, §5, §6.3, §6.4, §11, §12, §13 | Owns everything below and the `Controller`; dock order (busy panel bottom with its fixed height, input panel top, tabs fill; tab header padding (12, 4) via a `tabSizeHint` override), window geometry (§4.1), the start-up log line, OnShownAsync (map load, then `AutoTestRunner`), settings on close (cancel first; not in autotest). Keyboard (§6.3): an Enter no control used reaches `keyPressEvent` and presses `InputPanel::trigger_accept()` (grids consume Enter; a focused push button is clicked); an application event filter turns Esc in this window into `BusyPanel::request_cancel()` while working and no popup (drop-down, completer, calendar, menu) is open (D-11, WF-3). |
| `Controller` (QObject) | 01 §5, §6.4, §11, §12.2 | The MainForm flows as coroutines (see below): LoadCatalogAsync, PrepareAsync / ResolveAsync (+ chooser), SearchOutbound/Return, PlanReturnFromAsync, RunLegAsync (plan, 0-request path, budget gate, streaming, final status), OnCurrencyChanged, RecomputeTrips, UpdateTabTitles, Log, SetWorking. Modal boxes through injectable `ControllerDialogs`. |
| `AutoTestRunner` (QObject) | 01 §17.2 | The self-test script as a coroutine; screenshots through an injectable `Screenshotter` (default `save_window_png`). |
| `InputPanel` (QWidget) | 01 §4.2–4.5, §6.1–6.2 | Three wrapping `FlowLayout` rows + `_resolved` label; `PlaceCombo` ×2 (editable, completer, tooltips), `FractionalSpinBox` ×6, `DatePicker` ×4 (`ddd dd MMM yyyy`, clamped to [today, today+2y]), fare/currency combos, two styled magenta search buttons. Applies `ui::DateRules` for coupling. Emits `search_outbound_requested`, `search_return_requested`, `currency_changed`, `nights_changed`. |
| `FlowLayout` (QLayout) | 01 §4.3 | Wrapping row layout (Qt "Flow Layout" example semantics, heightForWidth). |
| `FractionalSpinBox` (QAbstractSpinBox) | 01 §4.4 notes | Displays whole numbers (half away from zero), accepts typed fractions with the locale separator, keeps the fractional value; `int_value()` truncates toward zero. |
| `FlightGrid` (QWidget: top bar + QTableView) | 01 §8 | `FlightTableModel` (`KeyedTableModel` over `ui::FlightGridModel`, which owns the index vector, sorts, view lists, pick and RestoreSelection): a rebuild is one model reset; a streamed "All fares" batch lands at scattered sorted positions, so it is one `layoutAboutToBeChanged`/`layoutChanged` with persistent indexes following each row's `Idx` (a batch into an empty grid is one `beginInsertRows`); a header sort is one layout change. View combo (`activated` = user choice), summary label, row colouring, context menu, copy with a timer-driven clipboard retry, open URL, Enter/double-click open, self-test accessors. Side effects go through injectable `GridActions` (browser, clipboard, MessageBox). |
| `RoundTripGrid` (QWidget) | 01 §9 | Two checkboxes, summary, `RoundTripTableModel` (over `ui::RoundTripGridModel`), sort, open combined/both URLs, context menu. |
| `GridTableView` (QTableView), `GridRowDelegate` | 01 §8.1, §8.3, §9.1 | Shared table of both grids: DataGridView flags, fixed rows (font height + 9), fill column with a minimum, wrapping headers with auto height, intent signals (cell press before the current cell moves, double-click, Enter consumed incl. ShortcutOverride, navigation key-up, context menu from mouse/header/Menu key). The delegate paints selection in `SystemColors.Highlight` (or the model's `SelectionBackgroundRole`, the cheapest rows' RGB(40,140,70)) in every style, no hover, tooltips for truncated cells. |
| `BusyPanel` (QWidget) | 01 §10 | Renders `ui::BusyPanelModel` on a 40 ms `QTimer`: `SpinnerWidget`, `QProgressBar` (0..1001, marquee = range 0..0), elapsed (Courier New 9.5 pt bold), status (`ElidedLabel`, tooltip when cut), wait note (DarkOrange bold), Cancel; metrics from font metrics (§10.1); drives `TaskbarProgress`. ControlLight / ControlText come from `system_colour()` (GetSysColor), because Qt styles repaint palette roles (the `windows11` style makes Midlight white and draws a thin Fluent bar; `windowsvista` reproduces the WinForms bar). Esc must call `request_cancel()` (D-11). |
| `SpinnerWidget` | 01 §10.8 | QPainter arc/track/dot exactly as specified. |
| `TaskbarProgress` | 01 §10.9 | `ITaskbarList3` RAII wrapper with the state/value de-duplication rules. |
| `PlaceChooserDialog` (QDialog) | 01 §11.5 | Modal chooser, exact texts, `Use this place` default, Cancel rightmost, double-click = OK. |
| `LogView` | 01 §12 | Read-only `QPlainTextEdit`, Courier New 9 pt, `[HH:mm:ss] ` prefix (culture time separator), unlimited, scroll to end; background SystemColors.Window (viewport kept filled after the style's polish); `text()` keeps the CRLF text for `summary.json`. |

## Controller and threading (01 §7, §11, §14; 06 §2)

- `Controller` (QObject, UI thread) implements the flows of 01 §11 using ui_logic for texts and
  request building, and services for work. It owns `_working`, the per-phase `std::stop_source`,
  `_outResult/_retResult`, the catalog, a `JobRunner` and a `TaskScope`.
- Flows are C++ coroutines (`task.hpp`): each .NET async method is a `Task<T>` with the same steps
  in the same order — `co_await` a child flow or a job, try/catch around the awaits, an RAII
  `PhaseGuard` for the `finally` (dispose the stop source, SetWorking(false), End(UnexpectedErrorText)
  when still busy). Tasks are lazy and use symmetric transfer, so a flow continues into the next
  phase without an event-loop round trip (no second search can slip in between phases). Root flows
  (buttons, menu, start-up) are started by `TaskScope::spawn`; an escaping exception goes to
  `CrashReporter::report_current_exception` and the app keeps running (01 §3.2). Destroying the scope
  (window closed) destroys suspended frames; their guards see the shutdown and touch nothing.
- `JobRunner` (`job_runner.hpp`): `co_await runner.run(job_id, stop, work)` starts `work(channel)`
  on a new `std::jthread` (also for fully cached work) and resumes the flow on the UI thread with the
  result or the rethrown exception. `JobChannel::post` queues a closure through
  `QMetaObject::invokeMethod(runner, …, Qt::QueuedConnection)`: progress, row batches and the
  completion of a job travel in one FIFO queue. A job is live from its start until its completion is
  delivered (or its flow's frame is destroyed); events of a job that is not live are dropped. The
  caller's `std::stop_token` is linked to the job's own stop source; destroying the runner stops and
  joins its workers. The UI never waits for a worker; finished threads are joined when the next job
  starts.
- RunLegAsync: progress reports and partial batches go through one `JobMailbox<LegEvent>`
  (`job_runner.hpp`, D-32): a worker push posts a delivery only when none is pending, so a burst
  (a cached replay produces two events per request within milliseconds) is one queued call. The
  delivery hands the reports to `BusyPanel::report(span)` (Report(p) for each, one refresh) and
  `ui::NoticeLog`, the batches to `ui::LegStream::on_partial(job_id, …)`, in the worker's order; a
  150 ms `QTimer` takes the pending rows (`LegStream::take_pending`) into `FlightGrid::append` +
  "{N} fares so far…"; `finish()` before the final `SetResults` drops late batches (U-10).
  `Controller::leg_started(Leg)` marks the start of the fetch phase (autotest diagnostics).
- Modal input (chooser, MessageBoxes) runs nested event loops; the busy panel keeps animating; the
  controller reports `Waiting for your choice` with `waiting_for_user = true` directly (not posted)
  before the chooser opens.
- Place resolution runs on a worker; the lookup comes back through the job's completion and the
  UI shows the chooser (UI thread) when `ui::decide_resolution` says so, then continues the flow.

## `--autotest` (01 §17, DECISIONS D-30)

`AutoTestRunner` runs the §17.2 script after the startup map load, takes full-window screenshots
(`PrintWindow(hwnd, PW_RENDERFULLCONTENT)` into a DIB → `QImage` → PNG, cropped to the DWM extended
frame bounds), progress screenshots every 2200 ms (max 4), writes `summary.json` via ui_logic, and
closes unless `--keep-open`. Hidden companions `--data-root`, `--replay` and `--fixed-now`
(ui_logic) make it runnable offline on an isolated profile; `--no-pacing`, `--warm-cache`,
`--cancel-after-ms` and `--diagnostics` serve the 05 §5 acceptance checks (D-30):

- `--warm-cache`: the outbound search runs once before the script, so the script's search is a
  fully cached replay (zero requests).
- `--cancel-after-ms n`: n ms after `Controller::leg_started(outbound)` the runner sends Esc to the
  focused control (through the KeyPreview filter), clicks Cancel, sends Esc again and calls
  `BusyPanel::request_cancel` — one `cancel_requested` must follow.
- `--diagnostics`: `diagnostics.json` next to summary.json (`autotest_diagnostics.hpp`):
  `TimerGapProbe` (gaps of the 40 ms timer: start→tick, tick→tick, last tick→stop; the runner's
  screenshots excluded), `EventWatchdog` (exclusive time of every UI-thread dispatch via
  `Application::notify`), the cancel record, per-search requests and durations, the final grids
  against the stored results (rows not in the result, duplicate keys), display/DPI data and
  `layout_problems()` (`layout_check.hpp`: clipped, cut-off, overlapping widgets, cut texts, tabs
  that do not fit, Cancel fully visible) at every screenshot.

Capture details: before each screenshot the runner applies the pending layout requests (WinForms
lays out synchronously; a growing `_resolved` label needs several passes) and repaints the window
synchronously; `capture_window` then calls `GdiFlush` + `DwmFlush`, because `PW_RENDERFULLCONTENT`
reads the DWM surface and would otherwise mix the previous frame with the new one.

Replay scenarios live in `tests/replay/<scenario>/` (`replay.json` + body files; fixture copies and
synthetic timetables; `over_500` reads the frozen live map in place):
`named_destination` (03 §11 legs, return, `--then-currency HUF`), `anywhere_return_from_top`,
`not_served` (InvalidMarket), `same_area`, `over_500`, `cancel_waiting` (a 30 s server, cancelled
after 1.5 s), `cancel_retrying` (503, cancelled in the 5 s back-off). `heavy_cached` and
`cancel_cached` are generated by the checker from the live map: every route from Budapest, one fare
per day over 105 days (≈ 6500 fares, 186 requests), `--warm-cache --no-pacing` (cancelled at 0 ms
for `cancel_cached`). CTest runs the real exe on each (label `autotest`,
`tests/gui/check_autotest.py`, Python `-I`, serial, always `--diagnostics`), plus
`named_destination`, `cancel_retrying` and `heavy_cached` at 150 % and 200 % (`QT_SCALE_FACTOR`),
and checks summary.json, diagnostics.json (timer gap below 100 ms, 250 ms in Debug) and the PNGs;
the in-process tests (`ControllerFlows.*`, `AutoTestRunner.*`) use the same rules without their
delays. summary.json carries no URLs (C-07), so booking links are asserted in-process. Results:
`docs/RELEASE_CHECKS.md`.

## Build and deployment

- Development: vcpkg's app-local step copies the DLLs and `wizz_copy_qt_plugins`
  (`cmake/WizzQtDeploy.cmake`) the platform, style and image-format plugins post-build, so Debug and
  Release start from the build tree.
- Release package (06 §4, D-31): `cmake --preset msvc-x64-release` then
  `cmake --build --preset package` (build target `PACKAGE`, CPack ZIP, `cmake/WizzPackaging.cmake`)
  → `build/msvc-x64-release/package/WizzairHelper-<version>-win64.zip`: the exe; Qt Core/Gui/Widgets,
  `platforms/qwindows.dll`, `styles/qmodernwindowsstyle.dll`, `imageformats/` (vcpkg's
  `windeployqt`); libcurl, `z.dll`, brotli, pcre2-16, double-conversion, libpng, md4c
  (`file(GET_RUNTIME_DEPENDENCIES)`); the app-local MSVC runtime; `THIRD-PARTY-NOTICES.txt`,
  `README.txt` and `licenses/` (`packaging/`). `cmake --install build/msvc-x64-release --config
  Release --prefix <dir>` produces the same folder. BUILDING.md has the commands.
