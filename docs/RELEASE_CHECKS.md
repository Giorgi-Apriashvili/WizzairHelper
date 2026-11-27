# Release checks — WizzairHelper 1.0.0

The release record that 06 §5 asks for ("record OS, DPI, locale, test data and observed timer gap
with release results"): the 05 §5 desktop acceptance runs, the 05 §7.3 responsiveness
measurements, the DPI runs of 01 §15, and the verification of the release package (06 §4).

Date: 2026-11-16. Commands: `ctest --preset release` / `ctest --preset debug` (label `autotest` for
the end-to-end runs) and `cmake --build --preset package`; details in [`BUILDING.md`](../BUILDING.md).

## Published releases

| Version | Date | Commit | What it added |
|---|---|---|---|
| [1.0.0](https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v1.0.0) | 2026-11-16 | `8dbb08d` | The first release: the app, the per-user installer and portable ZIP, the daily update check and the application icon. |
| [2.0.0](https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v2.0.0) | 2026-11-16 | `3c399d2` | The grouped-cards look: inputs in four titled sections, a modern theme in light and dark that follows Windows, High Contrast respected, a new Tab order (DECISIONS D-53 to D-57). |
| [2.1.0](https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v2.1.0) | 2026-11-17 | `935cf43` | Smooth text: natural ClearType rendering, Segoe UI Variable a touch heavier on Windows 11 (DECISIONS D-58). |
| [2.2.0](https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v2.2.0) | 2026-11-18 | `be62626` | The Settings window: manual update check, automatic checks once a day / week / month, three choices for what happens with an update, reminders for a put-off update (DECISIONS D-59..D-63); the first work starts after Windows' open animation (D-65). |
| [2.3.0](https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v2.3.0) | 2026-11-18 | `edfd6af` | Fares reused per route and day and kept on disk (`fares.json`, at most 6 h), the "Reuse fares fetched within" setting, exchange rates fetched at start, the reused-fares Log line (DECISIONS D-66..D-70). |
| [2.3.1](https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v2.3.1) | 2026-11-19 | `1ded54e` | Fix: a retired Wizz Air API version answering HTTP 503 made every search fail "Wizz Air is busy"; the address is now looked up again on such a page and after 24 hours (DECISIONS D-71). |
| [2.4.0](https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v2.4.0) | 2026-11-19 | `3f9438d` | Homes on Airbnb: a button above each results table and a menu item open Airbnb's search around the destination city's centre for the row's stay, within 2 / 5 / 10 / 25 km set in Settings > Searches (DECISIONS D-72, D-73). |
| [2.5.0](https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v2.5.0) | 2026-11-19 | `29c9fad` | The update's button (Restart and update / Update now / Download) right under Check now in the Settings window, also after Later; the Settings window no longer grows for a two-line status (DECISIONS D-74). |
| [2.6.0](https://github.com/Giorgi-Apriashvili/WizzairHelper/releases/tag/v2.6.0) | 2026-11-27 | `b38bb7c` | A Theme switch (Auto / Dark / Light) beside Close in the Settings window (DECISIONS D-75); several selected results in the three tables, with Copy details (also Ctrl+C) and Copy as table (D-76); Tab into the install choices lands on the saved one. |

v2.6.0 verification: CI run 45903348462 on `8a22f35` (the Theme switch and the Tab fix), CI run 46042468342 on
`033360d` (several selected results) and the tag's run 46045084831 on `b38bb7c` passed on all three legs and published
the release. Locally the changes passed 1791 / 1791 and then 1809 / 1809 tests in Release (all end-to-end runs, two new
ones with a saved Theme); the Debug and clang-cl suites ran on CI only (a local run was stopped for low memory). 23 rules
of D-75 and the Tab fix and 21 of D-76 were each broken on purpose and a test failed every time. Independent reviews
found 5 problems in D-75 (among them a High Contrast focus frame all but invisible on the selected Theme segment) and 12
in D-76 (among them the selection and the current row parting after an arrow key while results stream, a Shift range
losing rows after a reload, a click during streaming keeping the old selection), all fixed before the release. The
Settings window with the switch (light, dark, High Contrast) and the grids with several selected rows (light, dark) were
rendered and checked by eye. `tools/release_check.ps1 -Tag v2.6.0`: three assets (installer 10,185,266 bytes, ZIP
11,794,803 bytes, `SHA256SUMS.txt`), both checksums match, the packaged `WizzairHelper.exe --version` prints
`WizzairHelper 2.6.0`, and `releases/latest` is v2.6.0. Not verified: switching Windows' app mode and a contrast theme by
hand with the installed app (the owner's display settings were not changed), a real Shift+click (a widget test cannot
synthesize it, D-76), a real update to 2.6.0 run by the installed app.

v2.5.0 verification: CI run 44578932427 on `96a6ca9` and the tag's run 44584889106 on `29c9fad` passed on all three legs and published the release. Locally the change passed 1770 / 1770 tests in Release (all end-to-end runs), Debug and with clang-cl 22.1.8; 9 rules of the change were each broken on purpose and a test failed every time; an independent review found one bug (a button beside "Downloading" that would have installed an older ready version), fixed before the release. The Settings window was rendered in light, dark and High Contrast at 100 % and 150 % and checked by eye. `tools/release_check.ps1 -Tag v2.5.0`: three assets (installer 10,203,494 bytes, ZIP 11,778,198 bytes, `SHA256SUMS.txt`), both checksums match, the packaged `WizzairHelper.exe --version` prints `WizzairHelper 2.5.0`, and `releases/latest` is v2.5.0. Not verified: a real update to 2.5.0 run by the installed app, and the new button with a real release (the tests script the updater).

v2.4.0 verification: CI run 44527961958 on `79ee5d4` (the merge `82675da` plus the discovery timeout of D-50) and the
tag's run 44531103430 on `3f9438d` passed on all three legs and published the release. Locally the merged code passed
1762 / 1762 tests in Release (all end-to-end runs), Debug and with clang-cl 22.1.8; 15 rules of the feature were each
broken on purpose and a test failed every time; an independent review found no bug, and its two fixes (a shortened
city name must be the city itself; no stay before today) are in. The owner opened three generated links in their browser
(2026-11-19: a 5 km box around central London and Budapest, and Santorini by name, 4-8 Dec, 2 adults): Airbnb kept the
dates, the guests and the area. `tools/release_check.ps1 -Tag v2.4.0`: three assets (installer 10,172,893 bytes, ZIP
11,775,078 bytes, `SHA256SUMS.txt`), both checksums match, the packaged `WizzairHelper.exe --version` prints
`WizzairHelper 2.4.0`, and `releases/latest` is v2.4.0. Not verified: the city-centre rule against the live geocoders
for every destination (replayed answers only), a real update to 2.4.0 run by the installed app.

v2.3.1 verification: CI run 44497505123 on the fix `4ea6d1a` and the tag's run 44499954681 on `1ded54e` passed on all
three legs and published the release. Locally the fix passed 1714 / 1714 tests in Release (all end-to-end runs), Debug and
with clang-cl 22.1.8; each new branch of D-71 was broken on purpose and a test failed every time. The cause was confirmed
with two live requests the owner approved (the homepage advertised 29.26.0, CloudFront answered the saved 29.25.0 with an
HTML 503). `tools/release_check.ps1 -Tag v2.3.1`: three assets (installer 10,190,638 bytes, ZIP 11,758,048 bytes,
`SHA256SUMS.txt`), both checksums match, the packaged `WizzairHelper.exe --version` prints `WizzairHelper 2.3.1`, and
`releases/latest` is v2.3.1. Not verified: the fix against the live site (it needs a version Wizz Air has just retired;
the tests replay CloudFront's page), a real update to 2.3.1 run by the installed app.

v2.3.0 verification: CI run 44394123991 on the merge `fe1ca23` and the tag's run 44398047292 on `edfd6af` passed on all
three legs and published the release. Locally the merged code passed 1700 / 1700 tests in Release (all 24 end-to-end runs,
including the new `autotest.restart_shifted`), 1676 / 1676 in Debug and with clang-cl. Measured with replays and fakes,
not live traffic: 3 routes x 3 chunks sent 9 requests first, 0 on a repeat, 3 shifted by one day, 0 after a restart within
the window; the heavy replay sent 186 timetable requests, then 62 when repeated a day later in its dates after a restart
(124 of 186 answered from reused fares). `tools/release_check.ps1 -Tag v2.3.0`: three assets (installer 10,189,274 bytes,
ZIP 11,756,129 bytes, `SHA256SUMS.txt`), both checksums match, the packaged `WizzairHelper.exe --version` prints
`WizzairHelper 2.3.0`, and `releases/latest` is v2.3.0. Not verified: the request counts against the live site (needs the
owner's consent), a real update run by the installed app.

v2.2.0 verification: the tag's CI run 44314475295 on `be62626` passed on all three legs (msvc-release with the 23
end-to-end runs, msvc-debug, clang-cl-release now with LLVM 22.1.8 like local work) and published the release; on the
runner `startup.windowAnimates` was false, so no start delay there. Before it, CI run 44183181258 on the merge `a531466`
failed in clang-cl only: an LLVM 20 coroutine miscompile, worked around in `b567a4f` (D-64). Locally the merged code
passed 1638 / 1638 tests in Release (one stray click on a test window reran clean 8 of 8), 1615 / 1615 in Debug and
with clang-cl. `tools/release_check.ps1 -Tag v2.2.0`: three assets (installer 10,174,261 bytes, ZIP 11,733,717
bytes, `SHA256SUMS.txt`), both checksums match, the packaged `WizzairHelper.exe --version` prints
`WizzairHelper 2.2.0`, and `releases/latest` is v2.2.0. Not verified: a real update run by the installed app, a real
High Contrast theme, a real Windows 10 and physical 150 % / 200 % monitors.

v2.1.0 verification: CI run 44076402542 on the merge `e65dcf4` and the tag's run 44080055528 on `935cf43` passed on
all three legs (msvc-release with the 23 end-to-end runs, msvc-debug, clang-cl-release); the tag's run published the
release. Locally the release commit passed 1501 / 1501 tests in Release, and the merge 1478 / 1478 in Debug and with
clang-cl. `tools/release_check.ps1 -Tag v2.1.0`: three assets (installer 10,154,146 bytes, ZIP 11,704,882 bytes,
`SHA256SUMS.txt`), both checksums match, the packaged `WizzairHelper.exe --version` prints `WizzairHelper 2.1.0`, and
`releases/latest` is v2.1.0. Not verified: a real Windows 10 (its fonts were simulated with `WIZZ_FONTS=windows10`),
a real High Contrast theme, physical 150 % / 200 % monitors, and an update run by the installed app.

v2.0.0 verification: the first tag on `903bfad` failed in CI and published nothing: `autotest.cancel_retrying` froze
the first retry note for 149 ms on the runner, a cold-disk read of the fallback font that the start-up warm-up
only measured (DECISIONS D-33). The warm-up now draws each font; the tag was deleted and set again on `3c399d2`.
Its CI run 43928927290 passed on all three legs (msvc-release with the 23 end-to-end runs, msvc-debug,
clang-cl-release) and published the release; locally the same commit passed 1492 / 1492 tests in Release,
1469 / 1469 in Debug and 1469 / 1469 with clang-cl. `tools/release_check.ps1 -Tag v2.0.0`: three assets
(installer 10,150,775 bytes, ZIP 11,700,967 bytes, `SHA256SUMS.txt`), both checksums match, the packaged
`WizzairHelper.exe --version` prints `WizzairHelper 2.0.0`, and `releases/latest` is v2.0.0. Not verified: the
update from an installed 1.0.0 to 2.0.0 (the first real run of the updater), a real Windows 10, a real High
Contrast theme, a live light/dark switch and physical 150 % / 200 % monitors.

v1.0.0 verification: the tag's CI run 43859397046 passed on all three legs (msvc-release with the 17
end-to-end runs, msvc-debug, clang-cl-release) and published the release; locally the same commit
passed 1446 / 1446 tests in Release, 1429 / 1429 in Debug and 1429 / 1429 with clang-cl.
`tools/release_check.ps1 -Tag v1.0.0`: three assets (installer 10,124,738 bytes, ZIP 11,663,110
bytes, `SHA256SUMS.txt`), both checksums match, the packaged `WizzairHelper.exe --version` prints
`WizzairHelper 1.0.0`, and `releases/latest` is v1.0.0. The measurements below were taken on
2026-11-16 before the CI hardening (DECISIONS D-50 to D-52). That work changed the tests and also
made the grids cheaper (a final rebuild reuses the rows already shown; grouped views no longer
sort every fare per batch). The figures below are from before those changes; 1.0.0 passes the
same limits in the runs above, but its gaps were not tabulated again.

## Environment

| Item | Value |
|---|---|
| OS | Windows 11 Enterprise 23H2, build 10.0.22631, x64 |
| Display | one monitor, 2560 × 1440, work area 2560 × 1392, system DPI 96 (100 %) |
| Scaling runs | 100 % native; 150 % and 200 % through `QT_SCALE_FACTOR=1.5 / 2` (device pixel ratio 1.5 / 2) |
| Locale | en-US (user culture, UI culture and system locale); the replays run on the fixed clock 2026-11-02T10:00:00Z, local zone UTC |
| Toolchain | Visual Studio 2026 (18) MSVC x64, CMake 4.4.4, Python 3.14.10 for the checker |
| Libraries | Qt 6.11.2 (qtbase, vcpkg), libcurl 8.23.0 (Schannel, brotli 1.2.0), zlib, GoogleTest 1.18.0 |
| Style / font | windowsvista, Segoe UI 9 pt |
| Network | none: every request answered by `--replay` (D-30) on an isolated `--data-root` |

## Test data

- `tests/replay/named_destination`, `anywhere_return_from_top`, `not_served`, `same_area`:
  copies of `fixtures/` (map_subset.json, 11 airports; timetable_v2_bud_ltn / bud_nap; FX
  Frankfurter 2026-11-02) plus the synthetic 03 §11 return fares.
- `tests/replay/over_500`: the frozen live route map of 2026-11-02 (189 airports, D-24).
- `tests/replay/cancel_waiting`: the first timetable request waits 30 s (slow server).
- `tests/replay/cancel_retrying`: the first timetable request gets HTTP 503 (5 s back-off).
- `heavy_cached`, `cancel_cached` (generated by `tests/gui/check_autotest.py`): every route from
  Budapest in the live map, one fare per day from 2026-11-03 to 2027-02-15 (42-day chunks):
  186 requests, 6510 fares, run with `--warm-cache --no-pacing` so the measured search is a fully
  cached replay. The in-process test `ControllerFlows.R_03_A_heavy_cached_replay_…` builds the same
  data in C++ (`tests/gui/heavy_replay.hpp`).

## Results

Suites: **1303 / 1303 passed in Release and in Debug** (skipped as before: `GridSnapshots` without
`WIZZ_GUI_SNAPSHOT_DIR`; in Release also the crash-reporter stack-trace test, which needs PDBs).
Debug and Release build without warnings (`/W4 /WX`).

### End-to-end `--autotest` runs (Release, `ctest --preset release -L autotest`)

Timer gap = longest interval of the BusyPanel's 40 ms timer without a tick while busy (start →
first tick, tick → tick, last tick → stop), the checker's own screenshots excluded. Busy dispatch =
the longest uninterrupted UI-thread event while busy (UI-loop watchdog, R-03). Limits: 100 ms
(05 §5); the Debug runs use 250 ms.

| Run | Final status | Wizz requests | Timer gap max / mean (ms) | Busy dispatch (ms) | Window (logical) | Layout states / problems | Cancel |
|---|---|---|---|---|---|---|---|
| named_destination | Return Done: 4 fares from 4 of 4 requests in 1 s. | 9 | 48.1 / 39.3 | 6.3 | 1304 × 812 | 9 / 0 | – |
| named_destination @150 % | same | 9 | 41.5 / 39.5 | 10.2 | 1306 × 804 | 9 / 0 | – |
| named_destination @200 % | same | 9 | 42.7 / 39.2 | 14.4 | 1266 × 676 | 9 / 0 | – |
| anywhere_return_from_top | Return Done: 3 fares from 1 of 1 requests in 1 s. | 8 | 41.2 / 39.1 | 4.7 | 1304 × 812 | 8 / 0 | – |
| not_served | Outbound Done: 0 fares from 1 of 1 requests in 1 s. | 3 | 40.5 / 38.8 | 5.8 | 1304 × 812 | 2 / 0 | – |
| same_area | Start and destination are the same area (0 km apart). … | 2 | 0.8 | 0.4 | 1304 × 812 | 2 / 0 | – |
| over_500 | This search would need 6794 requests … (about 148 min). … | 2 | 0.6 | 0.4 | 1304 × 812 | 2 / 0 | – |
| cancel_waiting | Outbound Cancelled: 0 fares from 0 of 3 requests in 1 s. | 3 | 40.7 / 38.5 | 4.1 | 1304 × 812 | 2 / 0 | at 1500 ms, busy, 1 transition |
| cancel_retrying | Outbound Cancelled: 0 fares from 0 of 3 requests in 1 s. | 3 | 40.9 / 38.7 | 3.2 | 1304 × 812 | 3 / 0 | at 2500 ms (in the back-off), busy, 1 transition |
| cancel_retrying @150 % | same | 3 | 40.8 / 38.9 | 13.8 | 1306 × 804 | 3 / 0 | 1 transition |
| cancel_retrying @200 % | same | 3 | 40.6 / 38.6 | 13.9 | 1266 × 676 | 3 / 0 | 1 transition |
| heavy_cached | Outbound Done: 6510 fares from 186 of 186 requests in 1 s. 2 note(s) in the Log tab. | 188 (2 + 186 warm-up, **0** for the cached search) | 33.7 / 15.4 | 6.3 | 1304 × 812 | 2 / 0 | – |
| heavy_cached @150 % | same | 188 | 34.3 / 17.5 | 10.9 | 1306 × 804 | 2 / 0 | – |
| heavy_cached @200 % | same | 188 | 34.8 / 15.4 | 6.4 | 1266 × 676 | 2 / 0 | – |
| cancel_cached | Outbound Cancelled: 0 fares from 0 of 186 requests … | 188 | 33.9 / 10.8 | 6.4 | 1304 × 812 | 2 / 0 | at 0 ms, busy, 1 transition |

**Observed timer gap (Release): at most 48.7 ms over two full runs of the table above (the second
one: 33.1–48.7 ms), 33–42 ms under the heavy cached replay at 100/150/200 %, no gap above 50 ms.** Debug (`ctest --preset debug -L autotest`, debug Qt):
101–132 ms under the heavy cached replays, 41–79 ms elsewhere, all within the Debug limit; the
in-process `ControllerFlows.R_03_…` passes the 100 ms limit in Release.

Checked in every run (`check_autotest.py`): summary.json fields and key order, warnings, the Log
lines, request counts (the currency switch of named_destination adds none: 9 = 2 + 3 + 4), the
screenshots, no settings.json and no errors.log in the isolated profile, and from diagnostics.json:
timer gap, busy dispatch, the final Outbound / Return tables equal to the stored results (no row of
an earlier search, no late batch, no duplicate), layout problems none and Cancel fully visible in
every captured state, the window inside its screen's work area. Booking links are asserted
in-process (summary.json has no URL field, 01 §17.3).

### Cancellation (05 §5)

| Point | Scenario | Outcome |
|---|---|---|
| While waiting for a request | cancel_waiting: Esc, Cancel, Esc, handler at 1.5 s of a 30 s wait | `Cancelling… (finishing the current request)`, Cancel disabled, one `cancel_requested`; the wait was interrupted (search ended ≈ 1.5 s); result Cancelled, 0/3 |
| While retrying | cancel_retrying: at 2.5 s, inside the 5 s back-off after a 503 | Progress shot at 2.2 s shows `Wizz Air is busy (HTTP 503) — retry 1 of 3` with `⏳ retrying in 5.0 s`; one transition; the retry never went out (3 requests) |
| Replaying cached results | cancel_cached: at 0 ms after the cached fetch phase began | 1 transition in every run; 11 Release runs ended inside the cached fetch loop after 0, 1 (6×), 2 (3×) or 12 of 186 requests, with the grid equal to the partial result ("… cancelled early") |

Esc and Cancel pressed together always produced exactly one cancellation transition. In-process
tests cover the same (`AutoTestRunner.D_30_Cancel_after_ms_…`, `MainWindow.Esc_while_working_…`,
`BusyPanelTest.Cancel_and_esc_share_one_idempotent_handler`).

### DPI (01 §15)

Runs at 150 % and 200 % (above): no clipping, overlap, cut text or hidden Cancel in any captured
state (layout check at every screenshot, plus the PNGs inspected by eye: input rows, tab headers,
grids, round trips, Log, busy panel with the retry note). At 200 % the default L(1320) × L(820)
window is clamped to the logical work area (1280 × 696) and stays above the L(980) × L(600)
minimum. Artefacts of the simulation, not of the app: the title bar is drawn by Windows at the
system DPI (small caption text), and native theme parts (check boxes, spin and combo arrows) keep
their 96-DPI size because `QT_SCALE_FACTOR` does not change the DPI Windows reports.

### Release package (06 §4)

`cmake --build --preset package` → `WizzairHelper-1.0.0-win64.zip`, 11.5 MB, 34 files: the exe
(ProductName and FileDescription "WizzairHelper", 1.0.0.0), Qt6Core/Gui/Widgets,
`platforms/qwindows.dll`, `styles/qmodernwindowsstyle.dll`, `imageformats/qgif, qico`, libcurl,
`z.dll`, brotlicommon/brotlidec, pcre2-16, double-conversion, libpng16, md4c, the app-local MSVC
runtime (msvcp140*, vcruntime140*, concrt140), THIRD-PARTY-NOTICES.txt, README.txt and eight
license files. Verified by unpacking it to a new folder on another drive (outside the build tree)
and running, with `PATH` reduced to `C:\Windows\System32;C:\Windows` and a fresh `--data-root`:
named_destination, cancel_retrying, heavy_cached, cancel_cached and anywhere_return_from_top — all
passed (timer gaps 33.9–43 ms). The running process loaded only its own DLLs and plugins, the
app-local MSVC runtime and Windows' `icu.dll` (plus an overlay hook the test machine injects into
every process). `cmake --install build/msvc-x64-release --config Release --prefix <dir>` yields the
same 34 files.

## Defects found and fixed

| Defect | Evidence | Fix |
|---|---|---|
| The UI froze for 0.2–0.4 s when the first retry notice appeared | cancel_retrying, Release: 400.9 ms timer gap at `Wizz Air is busy (HTTP 503)`; an isolated measurement showed 180–410 ms for the first layout of "⏳" (Windows font fallback) and 0 ms afterwards | `FontWarmup` resolves the fallback on a worker during start-up; `main` waits for it before showing the window (D-33). After: 40.9 ms |
| A fast stream of worker events starved the 40 ms timer | heavy_cached @200 %, Release: 79–151 ms gaps although no event took more than 2.7 ms (watchdog); 62 ms at 100 % | `JobMailbox`: one queued delivery per burst of progress reports and row batches; `BusyPanel::report(span)` refreshes once per delivery (D-32). After: 34–42 ms at 200 % |
| The package carried 15.7 MB of D3D12 shader compilers | `dxcompiler.dll` / `dxil.dll` copied by windeployqt from the Windows SDK on MSBuild's PATH | `--no-system-dxc-compiler` (D-31) |

## Not verified here

- A physical 150 % / 200 % monitor and moving the window between monitors with different DPI
  (05 §5 manual step): only one 100 % monitor was available, so scaling was simulated with
  `QT_SCALE_FACTOR`, which leaves native theme parts at their 96-DPI size.
- A machine without Visual Studio: the package was run with a minimal `PATH` from a fresh folder
  and loaded no DLL from the build tree or vcpkg, but on a machine that also has the VC++
  redistributable in System32 (the app-local copies were the ones loaded).
- Taskbar progress at 150 % / 200 % (01 §15) was not captured: `PrintWindow` shows the window only.
- Live Wizz Air traffic: CI uses fixtures only (06 §5); no live check was run.
- Reused fares (2.3.0, DECISIONS D-66..D-70) against the real site, by hand and only with the
  owner's consent for live traffic: a search, the same search again (no timetable request), the
  search moved by one day (one request per route, for the new last day), the app restarted within
  the reuse window (no timetable request, the Log line with the fares' age) and after it, and the
  exchange rates fetched at start (the first search shows no rate download). The offline
  equivalents are `SearchCache.Request_counts_for_repeated_shifted_and_restarted_searches` and the
  end-to-end `restart_shifted` run.
- Homes on Airbnb (2.4.0, DECISIONS D-72, D-73) against the live geocoders: the city-centre rule was simulated on
  every station of the live map and geocoded with replayed answers only; a live sweep of all destinations would send
  about 190 to 400 Photon requests and needs the owner's consent. Airbnb's links were checked by the owner in a
  browser (v2.4.0 above); Airbnb documents them nowhere, so they can change without notice.
- Under `--fixed-now` the elapsed time stays `0:00` and summary `elapsedSeconds` is 0 (D-30 fixed
  clock), so the screenshots show no running time.
