# WizzairHelper — desktop application design (Qt 6 Widgets)

How the 01 desktop experience maps onto Qt. Behaviour is defined by 01 (and 06 §2, 07); the pure
rules live in `wizzairhelper_ui_logic` (Qt-free, unit-tested). Widgets in `src/app` are thin: they
render ui_logic view state, forward user input, and never contain spec rules that can be tested
without a window.

## Process and shell (01 §3, §15, §17)

- `main.cpp`: install the crash handler; parse `--autotest` options (ui_logic, from the UTF-16
  command line so `--to ""` and non-ASCII places survive) *before* `QApplication` — a bad date or
  `--fixed-now` goes to the crash box before any window; the reporter is pointed at `--data-root`
  first (`AutoTestOptions::parse_data_root`), so that record lands in the isolated profile, and
  at the `--fixed-now` clock after parsing; `PassThrough` rounding; `Application`. Per-monitor
  DPI v2 comes from the application manifest (`resources/WizzairHelper.manifest`), so the early
  crash box is DPI aware too (Qt finds the awareness it would set);
  `style::apply_application_look()` (the look of D-53: Fusion under `ModernStyle`, the light, dark or
  High Contrast tokens Windows asks for, Segoe UI Variable Text 10 pt drawn natural and slightly
  heavier with an event filter that keeps every widget's font so, D-58; it then follows Windows'
  changes, D-56); `FontWarmup` (a worker resolves the busy panel's "⏳" font fallback and the look's
  fonts, 0.2–0.4 s the first time, D-33); `AppServices::create`
  (`ServiceConfig::for_run`: CurlTransport / ReplayTransport, SystemClock / FixedClock, user
  locale, data-root-aware `FileCache`s, `--no-pacing`); `AppSettings::load`; the window is built;
  in a normal run (no `--autotest` / `--replay`, `ui::updates_allowed`) `MainWindow::enable_updates`
  with `UpdateHooks::system` (D-45); `FontWarmup::wait()`; `MainWindow::show_main_window()`; `exec()`.
  Right after the crash handler, `main` creates the installer's `AppMutex` (`WizzairHelper.AppMutex`,
  D-47), which the updater releases just before it starts an installer.
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
    log then starts at the throw site. `std::rethrow_exception` is a throw of its own, so `Task`
    and `JobAwaiter` carry a coroutine's or worker job's exception as a `TracedException` (the
    exception plus its recorded throw site), whose `rethrow()` hands that site to the tracer: a
    record still starts where the exception was first thrown, also on a worker. MSVC keeps
    terminate handlers per thread (`CrashReporter::install_terminate_handler()` for worker threads).
  - The dialog is injectable (tests); `set_data_root()` / `set_clock()` apply `--data-root` and
    `--fixed-now`.
- Logical pixels: Qt widget coordinates are already DPI-independent, so 01's `L(x)` is just `x`.
  Settings `WindowWidth/Height` are *device* pixels: multiply/divide by `devicePixelRatio()`.
- Window size (01 §4.1, §13): a WinForms `Size` / `Bounds` is the whole window rectangle, the
  invisible resize borders of Windows 10/11 included, while Qt sizes the client area and reports
  only the visible frame (the title bar). `MainWindow::apply_start_geometry` therefore computes the
  full frame with `AdjustWindowRectExForDpi` (`window_placement.hpp`): `GetWindowRect` is exactly
  L(1320) × L(820) (or the saved size, clamped to [L(980) × L(600), work area of the cursor's
  screen]), centred in that work area but never above or left of it (`CenterToScreen`); the
  minimum is the whole window too. Closing saves `(WindowState == Normal) ? Bounds :
  RestoreBounds` — `GetWindowRect` in the normal state (an Aero-snapped window included),
  `GetWindowPlacement().rcNormalPosition` when maximised or minimised — so the size round-trips.

## Widgets (one class per file in `src/app`)

| Class | Spec | Responsibility |
|---|---|---|
| `MainWindow` (QMainWindow) | 01 §4, §5, §6.3, §6.4, §11, §12, §13 | Owns everything below and the `Controller`; dock order (busy panel bottom with its fixed height, input panel top, the hidden-by-default update bar under it (D-46), tabs fill; tab header padding (12, 4) via a `tabSizeHint` override), window geometry (§4.1), the start-up log line, OnShownAsync (map load, then `AutoTestRunner`), settings on close (cancel first; not in autotest), then the updater's exit step (D-47); `enable_updates` (refused in autotest). The `SettingsButton` is the tab widget's top-right corner widget and follows the tab bar in the Tab order; `open_settings` opens the window-modal `SettingsDialog` (or raises the open one) with `settings_.updates` and the update controller's status, following `status_changed` while it is open (`ui::updates_off_view()` without a controller); `apply_update_options` keeps a change, passes it to `UpdateController::set_options` and saves settings.json at once (not in autotest); `closeEvent` closes the Settings window first (D-63). Keyboard (§6.3): an Enter no control used reaches `keyPressEvent` and presses `InputPanel::trigger_accept()` (grids consume Enter; a focused push button is clicked); an application event filter turns Esc in this window into `BusyPanel::request_cancel()` while working and no popup (drop-down, completer, calendar, menu) is open (D-11, WF-3). |
| `Controller` (QObject) | 01 §5, §6.4, §11, §12.2 | The MainForm flows as coroutines (see below): LoadCatalogAsync, PrepareAsync / ResolveAsync (+ chooser), SearchOutbound/Return, PlanReturnFromAsync, RunLegAsync (plan, 0-request path, budget gate, streaming, final status), OnCurrencyChanged, RecomputeTrips, UpdateTabTitles, Log, SetWorking. Modal boxes through injectable `ControllerDialogs`. |
| `AutoTestRunner` (QObject) | 01 §17.2 | The self-test script as a coroutine; screenshots through an injectable `Screenshotter` (default `save_window_png`). |
| `InputPanel` (QWidget) | 01 §4.2–4.5, §6.1–6.2, D-53, D-54 | Four `SectionCard`s in a 2 x 2 grid (Where, Travellers & prices, Outbound, Return; `TrailingRowLayout` rows move a search button under its row when it does not fit) + the `_resolved` label with an info icon; the Tab order follows the cards (D-54); `PlaceCombo` ×2 (editable, completer, tooltips), `FractionalSpinBox` ×6, `DatePicker` ×4 (`ddd dd MMM yyyy`, clamped to [today, today+2y]), fare/currency combos, two styled magenta search buttons. Applies `ui::DateRules` for coupling. Emits `search_outbound_requested`, `search_return_requested`, `currency_changed`, `nights_changed`. |
| `SectionCard`, `GlyphLabel`, `TrailingRowLayout` | D-53 (replace 01 §4.3's FlowLayout rows) | A titled input card (rounded surface, accent icon, upper-case caption, optional muted hint); one icon-font glyph painted from its outline; a row of leading widgets with a trailing one at the right that wraps under the row when the width is short (heightForWidth). |
| `ModernStyle` (QProxyStyle over Fusion), `style::` (`ui_style.hpp`) | D-53..D-56 | Paints the look from `style::theme()`: inputs, buttons, check boxes, radio buttons, tabs, headers, scroll bars, list items, menus, popup frames, tooltips, focus rings; the calendar's arrows as chevron icons. `ui_style` holds the light, dark and High Contrast tokens, the fonts, the icon drawing, the choice of the look (High Contrast, WIZZ_COLOR_SCHEME, the Windows app mode), `refresh_appearance()` and the `ThemeNotifier` the widgets listen to. |
| `FractionalSpinBox` (QAbstractSpinBox) | 01 §4.4 notes | Displays whole numbers (half away from zero), accepts typed fractions with the locale separator, keeps the fractional value; `int_value()` truncates toward zero. |
| `FlightGrid` (QWidget: top bar + QTableView) | 01 §8 | `FlightTableModel` (`KeyedTableModel` over `ui::FlightGridModel`, which owns the index vector, sorts, view lists, pick and RestoreSelection): a rebuild is one model reset; a streamed "All fares" batch lands at scattered sorted positions, so it is one `layoutAboutToBeChanged`/`layoutChanged` with persistent indexes following each row's `Idx` (a batch into an empty grid is one `beginInsertRows`); a header sort is one layout change. View combo (`activated` = user choice), summary label, row colouring, context menu, copy with a timer-driven clipboard retry, open URL, Enter/double-click open, self-test accessors. Side effects go through injectable `GridActions` (browser, clipboard, MessageBox). |
| `RoundTripGrid` (QWidget) | 01 §9 | Two checkboxes, summary, `RoundTripTableModel` (over `ui::RoundTripGridModel`), sort, open combined/both URLs, context menu. |
| `GridTableView` (QTableView), `GridRowDelegate` | 01 §8.1, §8.3, §9.1, D-53 | Shared table of both grids: DataGridView flags, the 9 pt data font, fixed rows (font height + 12), no cell grid (a separator line under each row), fill column with a minimum, wrapping semibold headers with auto height, intent signals (cell press before the current cell moves, double-click, Enter consumed incl. ShortcutOverride, navigation key-up, context menu from mouse/header/Menu key). The delegate paints the selection as the theme's tint (or the model's `SelectionBackgroundRole`, the cheapest rows' deeper green) in every style, a 3 px bar at the left of the selected row (the accent while the grid has the focus, the muted text colour otherwise) and the accent bar on a focused grid's current row even when it is not selected (a ring round a grid without a current row), no hover, tooltips for truncated cells; its brushes are kept and renewed when the look changes. The summaries are `ElidedLabel`s (cut with "…", the full text as tooltip). |
| `BusyPanel` (QWidget) | 01 §10, D-34, D-53 | Renders `ui::BusyPanelModel` on a 40 ms `QTimer`: `SpinnerWidget`, `PanelProgressBar` (0..1001, marquee = range 0..0; it paints itself and is still at rest, D-34), elapsed (Cascadia Mono 10 pt semibold), status (`ElidedLabel`: 1 pt smaller when it does not fit, cut with "…" and a tooltip only when it does not fit then either), wait note (the theme's warning colour, the semibold face), Cancel; metrics from font metrics (§10.1); drives `TaskbarProgress`. A flat status bar in the theme's surface colour with a top border, the status in the theme's success and failure colours; every colour is taken again when the look changes. Esc must call `request_cancel()` (D-11). |
| `SpinnerWidget` | 01 §10.8 | QPainter arc/track/dot exactly as specified. |
| `TaskbarProgress` | 01 §10.9 | `ITaskbarList3` RAII wrapper with the state/value de-duplication rules. |
| `PlaceChooserDialog` (QDialog) | 01 §11.5 | Modal chooser, exact texts, `Use this place` default, Cancel rightmost, double-click = OK. |
| `LogView` | 01 §12, D-53 | Read-only `QPlainTextEdit`, Cascadia Mono 9.5 pt (Consolas), `[HH:mm:ss] ` prefix (culture time separator), unlimited, scroll to end; the theme's surface and text colours (viewport kept filled after the style's polish), no frame of its own (the results card frames it), a 3 px accent bar down its left while focused; `text()` keeps the CRLF text for `summary.json`. |
| `UpdateBar` (QWidget) | D-46, D-53, D-61 (not in the spec) | The slim card between the input panel and the tabs, inset like the input cards, hidden unless an update is offered: wrapping plain-text message (UI font, theme text) on a magenta tint with a 4 px accent at its left, a `SearchButton`-style action (`Restart and update` / `Update now` / `Download`; none while `Update now` checks or downloads) and a flat, text-sized `Later`; fixed-size buttons; renders `ui::UpdateBarView`, never takes the focus. |
| `UpdateController` (QObject) | D-45, D-47, D-59..D-62 | The updater's lifecycle over `ui::UpdateModel` and an `UpdateBackend` (`CoreUpdateBackend` = the services' `Updater` + `UpdateSchedule`): first evaluation at the first Shown (it deletes old downloads and offers again an update put off in an earlier session, from update-state.json and the downloads folder, without a request: `UpdateModel::remind`), then hourly (`QTimer`) with the user's options (`set_options`); one piece of work at a time — an evaluation, `check_now` (the Settings window's Check now), the check of `Update now` for such a remembered update, a download — on its own `JobRunner` workers, the rest queued; Log lines; bar refresh on results and on `Controller::working_changed`; `status_view()` and `status_changed` for the Settings window; `Later`; `Restart and update` and a completed `Update now` → `restart_requested` (the window closes); `on_window_closing` (from `closeEvent`, after the settings) releases the `AppMutex` and starts a verified installer through the injectable `InstallerLauncher` (`launch_detached`) when the install choice says so. |
| `SettingsButton` (QPushButton) | D-63 (not in the spec) | The flat gear `Settings` at the right end of the tab row (the tab widget's corner): as tall as the tab bar, the icon-font gear in the accent, the text in the secondary colour (the text colour under the mouse), the style's flat-button fill and focus ring inset like a tab's. |
| `SettingsDialog` (QDialog) | D-62, D-63 (not in the spec) | The Settings window: fixed width 600, height from its contents, window-modal, `Close` default, Enter clicks the focused push button, Esc closes; the focus waits on the status line while a focused Check now is disabled. One `SectionCard` "Updates": version line and `Check now` (`Checking…` while busy, one width), the status line (success / failure / secondary / muted colour, room for two lines), the automatic-check box with the frequency drop-down, the three install choices as radio buttons, the portable note. Renders `ui::UpdateStatusView` (`show_status`) and `update::UpdateOptions` (`show_options`); emits `options_changed` and `check_now_requested`; changes nothing itself. |

## The look (DECISIONS D-53..D-58)

Fusion under `ModernStyle` (`modern_style.hpp`), colour tokens in `style::Theme`
(`ui_style.hpp`), no style sheets. The 01 look (windowsvista, Segoe UI 9 pt, WinForms metrics and
SystemColors) is replaced on purpose; behaviour is not.

**Layout.** The input panel (16 px side margins, 6 px top) holds four `SectionCard`s in a 2 x 2
grid with an 8 px gap and column stretch 3:2: Where | Travellers & prices, Outbound | Return.
A card: rounded 8 px, 1 px border, padding (14, 6, 14, 8), a caption row (10 pt accent icon,
8 pt upper-case semibold caption, 9 pt muted hint; as tall without a hint as with one, so the rows
of cards side by side line up), 5 px to the body, 6 px between body rows.
Fixed widths: radius 76, dates 142, adults 64, fare 206, currency 88, nights 64; the place boxes
stretch (minimum 180). The resolved line follows 4 px below in 9 pt muted text. The results sit
4 px below in one rounded card under underline tabs (16, 4, 16, 8 around it), the gear `Settings`
at the right end of the tab row, as tall as the tabs (D-63); the grid's top bar
has padding (12, 4, 12, 4). Inputs and buttons are `ModernStyle::control_height` = max(26, line
height + 10) tall, the line as Qt lays it out (`ModernStyle::line_height`: the ascent plus the descent
rounded up, 18 px for the UI font without hinting, so 28 px at 100 %; D-58); grid rows are the 9 pt
data font's height + 12 (28 px at 100 %); header cells have 9 / 5 px padding around one line
(28 px). With these, 14 round trips are in view at the default 1320 x 820 window, with 2 px to
spare (`MainWindowLayout.*` checks it in CTest, `LookSnapshots` on the real platform); the prototype
had 12 and the windowsvista look 19.

**Fonts.** UI: Segoe UI Variable Text 10 pt; grid cells: Segoe UI Variable Text 9 pt with
semibold 10 pt headers; captions: Segoe UI Variable Small Semibold 8 pt, upper case, +0.8 px;
hints: 9 pt; log: Cascadia Mono 9.5 pt; elapsed time: Cascadia Mono 10 pt semibold; icons: Segoe
Fluent Icons, painted from the glyph outline (`QRawFont`, cached per glyph), because Qt's shaped
text renders nothing for these private-use code points. Windows 10: Segoe UI, Segoe UI Semibold,
Consolas (Cascadia Mono only with Windows Terminal) and Segoe MDL2 Assets (same code points).
Semibold is always the semibold family (`style::semibold_face()` for text that takes its size from
elsewhere: the Total cells, the wait note); a weight of 600 on Segoe UI Variable Text gives its Bold.

**Text rendering** (D-58). Every font is drawn natural and slightly heavier: `QFont::PreferNoHinting`
(DirectWrite ClearType natural symmetric, grid fitting off: curves smoothed vertically too, glyphs at
fractional positions), and every face with a weight axis 50 units heavier than the face Qt picks
(`style::weight_axis_for`): Segoe UI Variable regular 450, semibold 650, Bold 700 (the axis ends
there), Cascadia Mono 450 / 650. The Windows 10 fallbacks have no weight axis and are plain natural.
ui_style's fonts carry it: each names its face (its families, and the regular ones their weight) and
has the hinting and the axis as its own, so `resolve()` keeps them and the font draws as built
whatever font it is resolved against (a model's Qt::FontRole, QPainter::setFont, a widget's own
font under its parent's). The application font is one of them, and `style::keep_text_rendering()`,
an application event filter, fits every widget's font on Polish and FontChange in every look: a
copy of a font keeps its axis, so a setBold() copy (the calendar's month and year buttons) or a
QFont built without ui_style would otherwise draw at the wrong weight. ModernStyle draws menu items
in a fitted font (a default action bold). A font the app derives for painting alone, with another
weight or family, goes through `style::fit_text_rendering()` too; it makes the axis the font's own
only when the font names its families (one that takes them from the font it is resolved against
takes that font's axis along). Labels, message boxes and tooltips are plain text: rich text makes
its bold (`<b>`, font-weight) in a text format of its own, which nothing fits, so it would draw at
the regular axis (`TextRendering.*` fails on a visible rich-text label). High Contrast keeps the
rendering. Unhinted metrics are fractional: lines are measured as Qt lays them out
(`ModernStyle::line_height`), and widths that Qt elides by a fraction of a pixel unrounded (ElidedLabel,
the grids' cut-cell tooltips).

**Tokens.** All text tokens are at least 4.5:1 on window, surface, surface_alt, input, selection,
hover and the update tint; accent lines, the input stroke, selected text against the input, the
scroll thumbs, the grids' row bars and the busy bar at least 3:1 (`Look.*` tests).

| Token | Light | Dark |
|---|---|---|
| window (behind the cards) | `#F3F2F6` | `#131217` |
| surface (cards, tables, log, status bar, popups) | `#FFFFFF` | `#1D1C22` |
| surface_alt (table header, calendar header row) | `#F8F7FA` | `#25242B` |
| input / input disabled | `#FFFFFF` / `#F4F3F6` | `#27262E` / `#201F25` |
| border / input border / hover / bottom stroke | `#E1DFE6` / `#C6C2CD` / `#9A95A3` / `#8A8594` | `#2F2D36` / `#474550` / `#6C6976` / `#85818F` |
| text / secondary / muted / disabled | `#1C1B21` / `#45424D` / `#67636F` / `#A39FAA` | `#EEECF2` / `#C9C6D1` / `#A39FAC` / `#6A6772` |
| accent (primary buttons, busy bar in light) / hover / pressed | `#C5006E` / `#B10063` / `#960054` | `#C5006E` / `#D41A80` / `#A8005E` |
| accent line (focus, selected tab, pills, icons, grid bar; busy bar and spinner in dark) / accent text (weekends) | `#C5006E` / `#C5006E` | `#F05AA8` / `#F772B8` |
| selection (items: grid rows, lists, menus, calendar days; with the text colour) / hover | `#FBE2EF` / `#F3F1F6` | `#4B1834` / `#33313B` |
| selected text in inputs and the log (with white text) | `#C5006E` | `#D41A80` |
| cheapest rows / selected | `#E7F5EB` / `#C6E9D0` | `#173022` / `#1F4C32` |
| success / failure / warning | `#1A7F37` / `#C42B1C` / `#9A5B00` | `#5CCB85` / `#FF7A6E` / `#F5B04A` |
| row separator, track, scroll thumb / hover | `#EEEDF1`, `#E6E4EA`, `#8A8594` / `#67636F` | `#2A2931`, `#34323C`, `#85818F` / `#A39FAC` |
| update tint | `#FBEAF3` | `#361629` |

The palette (`make_palette`) maps them to Qt's roles; Highlight is the selected-text colour with
white HighlightedText, so a selection in an input (the place box's inline completion, a select-all)
stands out. `ModernStyle` paints list items and calendar days with the selection tint and the text
colour instead (`CE_ItemViewItem`), like the grids' delegate and the menus.

**Surfaces.** Combo lists and the place completer draw their items through the style (hover fill,
tint, 3 px accent pill); popup frames are 1 px input borders on the surface. Menus: surface,
1 px border, items of the font height + 12 with a rounded tint and pill on the active one, separators in the border
colour. The calendar: tint navigation bar with chevron arrows, no week numbers, weekends in the
accent text colour, neighbouring-month days muted, days outside the allowed dates in the disabled
text colour, a ring on the current day, the month name without Fusion's menu arrow over it. Tooltips: the
surface (surface_alt in dark) with a 1 px border and the theme's text (the tooltip palette is set
with the application palette). Message boxes and the place chooser use the window colour, the
default button is primary. The Settings window (D-63): the window colour around one card, the
status line in the success or failure colour, radio buttons like the check boxes on a circle (an
accent circle with a white dot when checked), `Close` primary. Check boxes and radio buttons keep
3 px before their indicator for the left side of its focus ring; disabled ones are outlined in the
disabled text colour. Disabled inputs: input_disabled fill, border-coloured frame, disabled
text; disabled search buttons and default buttons: the input_disabled fill, an input-border outline
and the disabled text, like every other disabled button. Mnemonic underlines show while Alt is held
or when Windows' "Underline access keys" is on. Hover: a darker frame on inputs, a lighter / darker fill on
buttons, header sections and check boxes. Focus: see D-53.

**Changes of the look** (D-56). `apply_application_look()` installs the look once; Windows'
changes (`colorSchemeChanged`, `contrastPreferenceChanged`, `ThemeChange` on the main window)
schedule `refresh_appearance()`, which installs the new theme, style and palettes and emits
`ThemeNotifier::changed`. Every widget that copies a token into a palette, brush or text format
connects to it (`style::on_theme_change`, `style::set_text_tone`); custom-painted widgets read
`style::theme()` when they paint. High Contrast (D-55) switches to windowsvista and the system
palette, and back.

**High Contrast** (D-55). When Windows reports a contrast theme (`SPI_GETHIGHCONTRAST`), the app
switches to the plain native style (windowsvista, else Windows) and the system palette, and takes
every token from that palette: Window, WindowText, Base, Highlight, HighlightedText, Button and
GrayText. Nothing is tinted or mixed: muted text is the text colour, the status colours are the
text colour (the status texts say what happened), cheapest rows are semibold instead of tinted, the calendar has
no weekend colour, a search button is Highlight with HighlightedText (a disabled one a GrayText
outline), the spinner and the bar use opaque system colours. The app font stays. High Contrast
wins over `WIZZ_COLOR_SCHEME`.

**Switches** (D-56, D-58, BUILDING.md): `WIZZ_COLOR_SCHEME=light|dark` forces a scheme (the title
bar follows), `WIZZ_HIGH_CONTRAST=1|0` overrides the contrast check, `WIZZ_FONTS=windows10` leaves out
the Windows 11 families. They are read at every evaluation of the look, so tests switch a running
window by changing them and calling `style::refresh_appearance()`. `WIZZ_TEXT_RENDERING=hinted|natural`
draws text as 2.0.0 did (its layout too at 100 %) or without the extra weight, for comparisons and bug
reports; it is read when a font is built, so it is set before the app starts (tests install the look
again).

**Tests** (D-57). The GUI tests run under this look (light, High Contrast pinned off; the test main
installs it like `main()`), on the minimal platform with its Windows font database
(`minimal:enable_fonts`), so their metrics are Windows' own (but for two fonts, D-57). `Look.*` covers the choice of the
look, a live switch of a running window (palettes, cached brushes, labels, calendars, the busy bar,
the update bar), the High Contrast fallback, the Windows 10 families, the WCAG contrast of
every token pair and what the colours stand for; `ModernStyle.*` the control, header, tab, scroll bar and item metrics the layout
relies on; `InputPanel.*` the cards (rows of side-by-side cards aligned to the pixel), the Tab order and
accessible names of D-54 and the minimum window; `FlightGridWidget.*` grabs the row separators,
the row bars and the ring of a focused grid;
`MainWindowLayout.*` the whole window at the client area of 980 x 600 (no clipping, 5 round trips
in view) and of 1320 x 820 (14 round trips; the Settings gear in the tab row's corner included, which
`layout_problems` checks like the laid-out widgets); `SettingsButton.*` and `SettingsWindow.*` the
gear (no height of its own, Tab order, look) and the Settings window (every state, layout, names,
Tab order, light / dark / High Contrast, each install choice end to end); `BusyPanelTest.An_idle_bar_is_completely_still_D_34`
the still bar; `TextRendering.*` the rendering in every font, widget, popup, dialog and grid cell
(default, natural, hinted, High Contrast, Windows 10 fonts), Qt's bold copies and the overrides. End
to end, `named_destination`, `cancel_retrying` and `heavy_cached` also run in dark, and
`named_destination` at the minimum window, and every run checks the look it showed and the text
rendering (`diagnostics.json`: the rendering, the UI font's hinting and weight axis).

**Review.** `wizzairhelper_gui_tests --gtest_filter=LookSnapshots.*` with
`WIZZ_GUI_SNAPSHOT_DIR=<dir>` on the real windows platform renders the window at the default and
the minimum size and every surface above in light and dark, the Windows 10 fonts and the High
Contrast fallback, and checks their layout; `LookSnapshots.Render_the_Settings_button_window_and_update_bars`
renders the gear, every state of the Settings window and the update bar of every install choice in
light, dark and High Contrast (run it with `QT_SCALE_FACTOR=1.5` and `2` too). `check_autotest.py --color-scheme`, `--window 980x600`,
`--fonts windows10` and `--text-rendering` do the same end to end.

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
  against the stored results (rows not in the result, duplicate keys), display/DPI data, the
  look in use (`environment.look`, the palette's window colour, the style, the UI font's family, the
  text rendering with the UI font's hinting and weight axis, and the face and weight axis of the UI,
  semibold, caption and monospace fonts, D-58) and
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
`named_destination`, `cancel_retrying` and `heavy_cached` at 150 % and 200 % (`QT_SCALE_FACTOR`) and
in the dark look (`.dark`; `named_destination.dark.scale200` too), `named_destination` at the
980 x 600 minimum window in light and dark (`.minimum`),
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
