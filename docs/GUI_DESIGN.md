# WizzairHelper — desktop application design (Qt 6 Widgets)

How the 01 desktop experience maps onto Qt. Behaviour is defined by 01 (and 06 §2, 07); the pure
rules live in `wizzairhelper_ui_logic` (Qt-free, unit-tested). Widgets in `src/app` are thin: they
render ui_logic view state, forward user input, and never contain spec rules that can be tested
without a window.

## Process and shell (01 §3, §15, §17)

- `main.cpp`: parse `--autotest` options (ui_logic) *before* `QApplication`; per-monitor DPI v2 is
  Qt 6's default (keep `Qt::HighDpiScaleFactorRoundingPolicy::PassThrough`); install the crash
  handler; load settings; create `MainWindow`; `exec()`.
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

## Widgets (one class per file in `src/app`)

| Class | Spec | Responsibility |
|---|---|---|
| `MainWindow` (QMainWindow) | 01 §4, §5, §6.3, §6.4, §11, §12, §13 | Owns everything below, the `Controller`, settings load/save on close, window geometry rules (§4.1), AcceptButton-follows-focus and Esc handling (§6.3), `set_working()` enable list (§6.4), tab titles (§5). |
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
  request building, and services for work. It owns `_working`, the per-phase `std::stop_source`, the
  active job ID, `_outResult/_retResult`, and the 150 ms flush `QTimer`.
- `JobRunner`: every job (map load, place resolution, leg search) runs on a worker (`std::jthread` or
  the service's own future); worker callbacks call `post(job_id, event)` which uses
  `QMetaObject::invokeMethod(receiver, lambda, Qt::QueuedConnection)` — one ordered channel per
  receiver. The UI drops events whose job ID is not the active one, or whose job has finished.
  Completion is observed through the queued channel, never by blocking on a future in the UI thread.
- Modal input (chooser, MessageBoxes) runs nested event loops; the busy panel keeps animating; the
  controller reports `Waiting for your choice` with `waiting_for_user = true`.
- Place resolution runs on a worker; if `needs_confirmation`, the worker result is posted back and the
  UI shows the chooser (UI thread), then continues the flow.

## `--autotest` (01 §17, DECISIONS D-30)

`AutoTestRunner` runs the §17.2 script after the startup map load, takes full-window screenshots
(`PrintWindow(hwnd, PW_RENDERFULLCONTENT)` into a DIB → `QImage` → PNG), progress screenshots every
2200 ms (max 4), writes `summary.json` via ui_logic, and closes unless `--keep-open`. Hidden
companions `--data-root` and `--replay` (ui_logic) make it runnable offline on an isolated profile.

## Build and deployment

- `windeployqt` runs post-build (Debug and Release) so the exe starts from the build tree; the
  release package (`cmake --build --preset package --target package` or an install step) contains
  the exe, Qt DLLs, `platforms/qwindows.dll`, styles plugin, libcurl/zlib/brotli DLLs, the MSVC
  runtime and `THIRD-PARTY-NOTICES.txt`.
