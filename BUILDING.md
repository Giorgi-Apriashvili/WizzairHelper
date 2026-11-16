# Building WizzairHelper

Windows x64 only. The specification is kept locally in `spec/` (not published; code comments cite
its chapters as "01 §10.3" etc.); the code conventions are in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), the deliberate
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
- Inno Setup 6 (only for the `dist` target, which builds the installer):
  `winget install JRSoftware.InnoSetup`. CMake looks for `ISCC.exe` on `PATH`, in
  `C:\Program Files (x86)\Inno Setup 6` and in `%LOCALAPPDATA%\Programs\Inno Setup 6`; elsewhere,
  configure with `-DWIZZ_ISCC=<path of ISCC.exe>`. Without it everything else builds, and `dist`
  fails and says so.

## Configure, build, test

```powershell
cmake --preset msvc-x64            # app + tests, build/msvc-x64
cmake --build --preset debug       # or: --preset release
ctest --preset debug               # or: --preset release
```

| Preset | What |
|---|---|
| `msvc-x64` | App, core/services/ui_logic libraries, conformance tests and GUI tests; build presets `debug`, `release` and `dist` (the release artefacts) |
| `msvc-x64-core` | Core, services, ui_logic and their tests, no Qt (`core-debug` / `core-release`) |
| `clang-cl-x64` | Same targets built with clang-cl 20+ and Ninja Multi-Config (build/test preset `clang-cl-release`); CI's second compiler. Needs `clang-cl` and `ninja` on `PATH` |
| `msvc-x64-release` | App only, no tests; build preset `package` = the `dist` target in this tree |
| workflow `full` / `core` / `release-package` | Configure + build (+ test) in one command: `cmake --workflow --preset full`; `release-package` = configure `msvc-x64` + build `dist` |

CI ([`.github/workflows/build.yml`](.github/workflows/build.yml)) runs the same presets: `release`
and `debug` with `ctest -LE autotest` (one test per CPU of the runner instead of the preset's 8),
the `autotest` label and `dist` in the Release leg
([`docs/RELEASING.md`](docs/RELEASING.md)).

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
- Label `timing`: the R-03 / 05 §5 timing tests of `wizzairhelper_gui_tests` (`FlightGridPerformance.*`,
  the progress timer under a replay). They run alone (RUN_SERIAL) and check the UI thread's own CPU
  time always and wall-clock time unless other processes held the CPUs (DECISIONS D-50), so they hold
  on a busy machine. To try that locally, pin the test binary to 2 logical CPUs beside 4 busy loops.
- `app.version*`: the real `WizzairHelper.exe --version` read through pipes, captured by Windows
  PowerShell, and its version resource (D-35, D-37). No window opens.
- `autotest.*` (label `autotest`): the real `WizzairHelper.exe` in its hidden self-test mode on
  replayed scenarios, checked by `tests/gui/check_autotest.py`. They open real windows one at a
  time and need an interactive desktop session. `ctest --preset debug -L autotest` runs only these,
  `-LE autotest` everything else.

## Release artefacts

```powershell
cmake --preset msvc-x64
cmake --build --preset dist        # = cmake --workflow --preset release-package
```

The `dist` build preset builds Release and the target `dist` (`cmake/WizzPackaging.cmake`,
`cmake/WizzDist.cmake`, D-38), which writes exactly two files to `build/msvc-x64/dist`:

| File | What |
|---|---|
| `WizzairHelper-X.Y.Z-win64.zip` | Portable package: one folder `WizzairHelper-X.Y.Z-win64` |
| `WizzairHelper-X.Y.Z-win64-setup.exe` | Per-user Inno Setup installer of exactly the same files ([`packaging/WizzairHelper.iss`](packaging/WizzairHelper.iss)) |

X.Y.Z is `project(VERSION)` in `CMakeLists.txt`, the single source of the version (D-37): the exe's
version resource and `WizzairHelper.exe --version` (D-35) take it from there too. The files are
staged with `cmake --install` in `build/msvc-x64/dist-stage` first: `WizzairHelper.exe`, the Qt
DLLs with `platforms/qwindows.dll`, `styles/qmodernwindowsstyle.dll` and `imageformats/` (deployed
by vcpkg's `windeployqt`), libcurl, zlib (`z.dll`), brotli and Qt's own dependencies, the
**app-local MSVC runtime** (no VC++ redistributable needed; the UCRT ships with Windows),
`LICENSE.txt` (MIT), `THIRD-PARTY-NOTICES.txt`, `README.txt` and `licenses/`. The same folder without packing:

```powershell
cmake --install build/msvc-x64 --config Release --prefix C:\Temp\WizzairHelper
```

`cmake --build --preset package` builds the same target in the app-only tree
(`cmake --preset msvc-x64-release` first) into `build/msvc-x64-release/dist`.

Unpack the ZIP anywhere and start `WizzairHelper.exe`, or run the installer: it installs per user
into `%LOCALAPPDATA%\Programs\WizzairHelper` without administrator rights, with a Start-menu entry
and an uninstaller. Either way the app keeps its data in `%LOCALAPPDATA%\WizzairHelper`. The
installer's silent update switches and `/RELAUNCH`, and how to try them locally, are in
[`docs/RELEASING.md`](docs/RELEASING.md); `docs/RELEASE_CHECKS.md` records the release verification.

## Releases and `tools/`

A `vX.Y.Z` tag makes CI build, test and publish a GitHub release with the ZIP, the installer and
`SHA256SUMS.txt`; [`docs/RELEASING.md`](docs/RELEASING.md) is the process. Its PowerShell helpers:

| Script | Use |
|---|---|
| `tools/ci_wait.ps1 -Sha <sha>` | Waits for a commit's CI run, then prints each job's result, minutes and failed step |
| `tools/tag_wait.ps1 -Tag vX.Y.Z` | The same for a tag's run (the one with the release job) |
| `tools/release_check.ps1 -Tag vX.Y.Z -Exe WizzairHelper.exe` | Verifies a published release: assets, `SHA256SUMS.txt`, the packaged exe's `--version`, `releases/latest` |

`-Repo` defaults to `Giorgi-Apriashvili/WizzairHelper`.

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

A run with `--autotest` or `--replay` on its command line never checks GitHub for updates (DECISIONS
D-45), so self-tests stay offline and their request counts unchanged.

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
