# Building WizzairHelper

Windows x64 only. The specification is in `00-README.md` … `07-formatting-and-invariants.md`;
the code conventions are in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), the deliberate
deviations in [`docs/DECISIONS.md`](docs/DECISIONS.md).

## Prerequisites

- Windows 10 1903 or later / Windows 11, x64 (the app uses the system `icu.dll`).
- Visual Studio 2026 (version 18) with "Desktop development with C++" (MSVC, Windows SDK).
- CMake 3.30 or later (4.x tested) on `PATH`; the presets use the "Visual Studio 18 2026" generator,
  so no developer command prompt is needed.
- Git with the `external/vcpkg` submodule (`git submodule update --init`). vcpkg builds the pinned
  dependencies from `vcpkg.json` on the first configure: qtbase 6.11.2, libcurl (Schannel, brotli),
  GoogleTest.
- Python 3 (optional) for the end-to-end `--autotest` checks; without it CMake skips them.

## Configure, build, test

```powershell
cmake --preset msvc-x64            # app + tests, build/msvc-x64
cmake --build --preset debug       # or: --preset release
ctest --preset debug               # or: --preset release
```

| Preset | What |
|---|---|
| `msvc-x64` | App, core/services/ui_logic libraries, conformance tests and GUI tests |
| `msvc-x64-core` | Core, services, ui_logic and their tests, no Qt (`core-debug` / `core-release`) |
| `msvc-x64-release` | App only, for the release package (`package`) |
| workflow `full` / `core` / `release-package` | Configure + build (+ test) in one command: `cmake --workflow --preset full` |

In a git worktree without the vcpkg submodule, point at another checkout's vcpkg; to reuse an
already-installed dependency tree without running vcpkg, also pass its installed directory:

```powershell
cmake --preset msvc-x64 -DCMAKE_TOOLCHAIN_FILE=<checkout>/external/vcpkg/scripts/buildsystems/vcpkg.cmake `
      -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR=<checkout>/build/vcpkg_prefetch
```

Warnings are errors (`/W4 /WX`). New source files are picked up by `CONFIGURE_DEPENDS` globs; re-run
the configure step after adding files.

### Test suites

- `wizzairhelper_tests`: the 05 conformance catalogue and the ui_logic tests (fixtures only, fixed
  clock, en-US).
- `wizzairhelper_gui_tests`: widget and flow tests on a `QApplication`, run by CTest with
  `QT_QPA_PLATFORM=minimal` (no window appears). Run the binary directly without that variable to
  use the real `windows` platform; `WIZZ_GUI_SNAPSHOT_DIR=<dir>` saves widget snapshots.
- `autotest.*` (label `autotest`): the real `WizzairHelper.exe` in its hidden self-test mode on
  replayed scenarios, checked by `tests/gui/check_autotest.py`. They open real windows one at a
  time and need an interactive desktop session. `ctest --preset debug -L autotest` runs only these,
  `-LE autotest` everything else.

## Release package

```powershell
cmake --preset msvc-x64-release
cmake --build --preset package     # = cmake --workflow --preset release-package
```

The `package` build preset builds Release and the CPack target `PACKAGE`:
`build/msvc-x64-release/package/WizzairHelper-1.0.0-win64.zip`. It contains `WizzairHelper.exe`,
the Qt DLLs with `platforms/qwindows.dll`, `styles/qmodernwindowsstyle.dll` and `imageformats/`
(deployed by vcpkg's `windeployqt`), libcurl, zlib (`z.dll`), brotli and Qt's own dependencies, the
**app-local MSVC runtime** (no VC++ redistributable needed; the UCRT ships with Windows),
`THIRD-PARTY-NOTICES.txt`, `README.txt` and `licenses/`. The same folder without the ZIP:

```powershell
cmake --install build/msvc-x64-release --config Release --prefix C:\Temp\WizzairHelper
```

Unpack the ZIP anywhere and start `WizzairHelper.exe`; it keeps its data in
`%LOCALAPPDATA%\WizzairHelper`. `docs/RELEASE_CHECKS.md` records the release verification.

## The hidden self-test mode and replays

`WizzairHelper.exe --autotest <outdir> [flags]` drives the real window through a fixed script
(01 §17): it searches, takes screenshots (`1-outbound.png`, `progress-N.png`, …), writes
`<outdir>/summary.json` and closes. The documented flags are `--from`, `--to` (`""` = anywhere),
`--from-radius`, `--to-radius`, `--out A:B`, `--ret A:B`, `--currency`, `--then-currency`,
`--return-from-top` and `--keep-open`. Hidden test-only companions (DECISIONS D-30) make it run
offline and reproducibly:

| Flag | Effect |
|---|---|
| `--data-root <dir>` | Settings, `errors.log` and caches in `<dir>` instead of `%LOCALAPPDATA%\WizzairHelper` |
| `--replay <dir>` | Every HTTP request is answered from `<dir>/replay.json` (format: `src/ui_logic/include/wizz/ui/replay_transport.hpp`) |
| `--fixed-now 2026-11-02T10:00:00Z` | Fixed clock, local zone UTC |
| `--no-pacing` | No 1 s spacing between Wizz requests |
| `--warm-cache` | Runs the outbound search once first, so the script's search is a cached replay |
| `--cancel-after-ms <n>` | Presses Esc and Cancel n ms after the outbound search starts fetching |
| `--diagnostics` | Writes `diagnostics.json` (40 ms timer gaps, UI-loop watchdog, cancel record, final grids, DPI, layout checks) |

Example (an isolated profile and a replayed network):

```powershell
.\WizzairHelper.exe --autotest out --data-root profile --replay tests\replay\named_destination `
    --fixed-now 2026-11-02T10:00:00Z --diagnostics --from Budapest --from-radius 200 `
    --to London --to-radius 30 --out 2026-11-10:2026-11-20 --ret 2026-11-12:2026-11-20 --then-currency HUF
```

The checker runs a named scenario and validates everything; `--scale 1.5` / `--scale 2` run it at
150 % / 200 % (`QT_SCALE_FACTOR`), and `--exe` can point at a packaged exe:

```powershell
python -I tests\gui\check_autotest.py --exe build\msvc-x64\src\app\Release\WizzairHelper.exe `
    --replay-root tests\replay --scenario heavy_cached --work-dir C:\Temp\autotest\heavy --scale 2
```

Scenarios: `named_destination`, `anywhere_return_from_top`, `not_served`, `same_area`, `over_500`,
`cancel_waiting`, `cancel_retrying`, and the generated heavy replays `heavy_cached` and
`cancel_cached` (every route from Budapest in the frozen live map, about 6500 fares).
