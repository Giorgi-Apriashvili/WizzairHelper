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
- Logical pixels: Qt widget coordinates are already DPI-independent, so 01's `L(x)` is just `x`.
  Settings `WindowWidth/Height` are *device* pixels: multiply/divide by `devicePixelRatio()`.

## Widgets (one class per file in `src/app`)

| Class | Spec | Responsibility |
|---|---|---|
| `MainWindow` (QMainWindow) | 01 §4, §5, §6.3, §6.4, §11, §12, §13 | Owns everything below, the `Controller`, settings load/save on close, window geometry rules (§4.1), AcceptButton-follows-focus and Esc handling (§6.3), `set_working()` enable list (§6.4), tab titles (§5). |
| `InputPanel` (QWidget) | 01 §4.2–4.5, §6.1–6.2 | Three wrapping `FlowLayout` rows + `_resolved` label; `PlaceCombo` ×2 (editable, completer, tooltips), `FractionalSpinBox` ×6, `DatePicker` ×4 (`ddd dd MMM yyyy`, clamped to [today, today+2y]), fare/currency combos, two styled magenta search buttons. Applies `ui::DateRules` for coupling. Emits `search_outbound_requested`, `search_return_requested`, `currency_changed`, `nights_changed`. |
| `FlowLayout` (QLayout) | 01 §4.3 | Wrapping row layout (Qt "Flow Layout" example semantics, heightForWidth). |
| `FractionalSpinBox` (QAbstractSpinBox) | 01 §4.4 notes | Displays whole numbers (half away from zero), accepts typed fractions with the locale separator, keeps the fractional value; `int_value()` truncates toward zero. |
| `FlightGrid` (QWidget: top bar + QTableView) | 01 §8 | `FlightTableModel` (QAbstractTableModel over an index vector; one `beginResetModel`/`beginInsertRows` per batch), view combo, summary label, header-click sort via ui_logic comparator (stable, nulls first, secondary collation), row colouring, explicit pick (identity), RestoreSelection, context menu, copy, open URL, Enter/double-click open, self-test accessors. |
| `RoundTripGrid` (QWidget) | 01 §9 | Two checkboxes, summary, `RoundTripTableModel`, sort, open combined/both URLs, context menu. |
| `BusyPanel` (QWidget) | 01 §10 | Renders `ui::BusyPanelModel` on a 40 ms `QTimer`: `SpinnerWidget`, `QProgressBar` (0..1001, marquee = range 0..0), elapsed (Courier New 9.5 pt bold), status (elided), wait note (DarkOrange bold), Cancel; metrics from font metrics (§10.1); drives `TaskbarProgress`. |
| `SpinnerWidget` | 01 §10.8 | QPainter arc/track/dot exactly as specified. |
| `TaskbarProgress` | 01 §10.9 | `ITaskbarList3` RAII wrapper with the state/value de-duplication rules. |
| `PlaceChooserDialog` (QDialog) | 01 §11.5 | Modal chooser, exact texts, `Use this place` default, Cancel rightmost, double-click = OK. |
| `LogView` | 01 §12 | Read-only `QPlainTextEdit`, Courier New 9 pt, `[HH:mm:ss] ` prefix (culture time separator), unlimited, scroll to end. |

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
