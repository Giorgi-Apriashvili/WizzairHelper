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
- Pillow and Microsoft Edge (optional), only to regenerate the application icon after editing its
  SVGs ([below](#the-application-icon)); the build uses the committed icon files and needs no
  image tools.
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
| `clang-cl-x64` | Same targets built with clang-cl (LLVM 22.1.8, the version CI pins) and Ninja Multi-Config (build/test preset `clang-cl-release`); CI's second compiler. Needs `clang-cl` and `ninja` on `PATH` |
| `msvc-x64-release` | App only, no tests; build preset `package` = the `dist` target in this tree |
| workflow `full` / `core` / `release-package` | Configure + build (+ test) in one command: `cmake --workflow --preset full`; `release-package` = configure `msvc-x64` + build `dist` |

clang-cl is the second compiler only (the shipped program is built with MSVC). CI's
`clang-cl-release` leg installs the same LLVM as local work, **22.1.8** (`LLVM_VERSION` and
`LLVM_SHA256` at the top of `.github/workflows/build.yml`; the official `LLVM-22.1.8-win64.exe`,
cached and checked against its published SHA-256), not the runner image's own LLVM, so that a
compiler difference can't make CI and a local build disagree. Install that LLVM locally
(`LLVM-22.1.8-win64.exe` from https://github.com/llvm/llvm-project/releases/tag/llvmorg-22.1.8); to move
both, upgrade locally first, then change the two lines in `build.yml`. Known gaps:

- `std::throw_with_nested` captures no inner exception (`std::current_exception()` is empty in the
  `nested_exception` constructor), so `ExceptionDescription.Nested_exceptions_are_inner_exceptions`
  is skipped under clang.
- Before LLVM 22.1.7, a coroutine's catch parameter used in a loop that also suspends may be moved
  into the coroutine frame, where the C++ runtime never stores the exception: the handler reads
  garbage and crashes (LLVM issue 153949, fixed by pull request 186728). Coroutine flows therefore
  bind no exception in such a handler (`catch (...)` and a plain function, `src/app/lib/task.hpp`,
  [DECISIONS D-64](docs/DECISIONS.md)). 22.1.7 and later do not show this, and CI now uses 22.1.8;
  the rule stays, for any older clang-cl. To try another LLVM version without installing it,
  extract its release archive (`clang+llvm-<version>-x86_64-pc-windows-msvc.tar.xz`) anywhere and
  configure a separate build directory like `clang-cl-x64`, with `CMAKE_C_COMPILER` and
  `CMAKE_CXX_COMPILER` set to its `bin\clang-cl.exe` (inside a developer prompt, with Ninja).

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
- `wizzairhelper_gui_tests`: widget and flow tests on a `QApplication`, under the app's own look
  (light, High Contrast off; `WIZZ_COLOR_SCHEME=dark` runs them all in the dark look), run by CTest
  with `QT_QPA_PLATFORM=minimal:enable_fonts` (no window appears; the minimal platform with Windows'
  fonts, so the layout tests measure real text — plain `minimal` draws every glyph as a box, and the
  layout tests then fail). Run the binary directly without that variable to use the real `windows`
  platform; `WIZZ_GUI_SNAPSHOT_DIR=<dir>` saves widget snapshots, and
  `LookSnapshots.*` then renders the whole look for a review: the window at the default and the
  minimum size and every popup, dialog, hover and focus state in light and dark, the Windows 10
  fonts and the High Contrast fallback (DECISIONS D-53..D-56), checking their layout and that 14
  round trips are in view. The `AppIcon` tests also read the committed icon files and the icon
  group of the `WizzairHelper.exe` of the same configuration, which the build makes first.
- Label `timing`: the R-03 / 05 §5 timing tests of `wizzairhelper_gui_tests` (`FlightGridPerformance.*`,
  the progress timer under a replay). They run alone (RUN_SERIAL) and check the UI thread's own CPU
  time always and wall-clock time unless other processes held the CPUs (DECISIONS D-50), so they hold
  on a busy machine. To try that locally, pin the test binary to 2 logical CPUs beside 4 busy loops.
- `app.version*`: the real `WizzairHelper.exe --version` read through pipes, captured by Windows
  PowerShell, and its version resource (D-35, D-37). No window opens.
- `autotest.*` (label `autotest`): the real `WizzairHelper.exe` in its hidden self-test mode on
  replayed scenarios, checked by `tests/gui/check_autotest.py`. They open real windows one at a
  time and need an interactive desktop session. A name's suffixes are its variants: `.scale150` /
  `.scale200` (QT_SCALE_FACTOR), `.dark` (the dark look), `.themedark` / `.themelight` (that Theme
  choice saved in the run's settings.json, DECISIONS D-75) and `.minimum` (the 980 x 600 window), e.g.
  `autotest.named_destination.dark.scale200`. Every run also checks that the app showed the look it
  was asked for: High Contrast, else `WIZZ_COLOR_SCHEME`, else a Dark or Light Theme setting, else the
  Windows app mode. `ctest --preset debug -L autotest` runs only these,
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

## The application icon

`src/app/resources/icon/` holds the icon's two sources and everything made from them, all
committed ([DECISIONS D-51](docs/DECISIONS.md#application-icon)):

| File | What |
|---|---|
| `WizzairHelper.svg` | The design, drawn for 40 px and up |
| `WizzairHelper-small.svg` | The same design hand-tuned for 16 to 32 px (only the ring's front arc, bolder strokes, a taller pin) |
| `WizzairHelper-<N>.png` | N = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256: each drawn from its SVG at that size |
| `WizzairHelper.ico` | Those ten PNGs, byte for byte, as PNG-compressed icon entries |

The `.ico` is the exe's icon (`IDI_ICON1` in `src/app/resources/WizzairHelper.rc`: Explorer,
shortcuts, the taskbar, Apps & Features) and the installer's (`SetupIconFile`); the PNGs are
embedded as the Qt resource `:/icons/` and make the window icon (title bar, Alt+Tab, taskbar
button). After editing an SVG, regenerate the other files and commit them together:

```powershell
python -I tools\make_app_icon.py
```

It needs Python 3.10+ with Pillow installed for that interpreter (`python -m pip install pillow`;
`-I` ignores per-user site-packages) and Microsoft Edge, which draws every size headless from the
vector art on a transparent background (`--edge <msedge.exe>` if it is not in the usual place).
The same SVGs, Edge and Pillow give byte-identical files, so a run without changes leaves
`git status` clean; `--out <dir>` writes the files somewhere else instead, e.g. to compare them.

## The hidden self-test mode and replays

`WizzairHelper.exe --autotest <outdir> [flags]` drives the real window through a fixed script
(01 §17): it searches, takes screenshots (`1-outbound.png`, `progress-N.png`, …), writes
`<outdir>/summary.json` and closes. The documented flags are `--from`, `--to` (`""` = anywhere),
`--from-radius`, `--to-radius`, `--out A:B`, `--ret A:B`, `--currency`, `--then-currency`,
`--return-from-top` and `--keep-open`. Hidden test-only companions (DECISIONS D-30) make it run
offline and reproducibly:

| Flag | Effect |
|---|---|
| `--data-root <dir>` | Settings, `errors.log` and caches in `<dir>` instead of `%LOCALAPPDATA%\WizzairHelper`; the reused fares are kept in `<dir>\cache\fares.json` too (without `--data-root` an `--autotest` run keeps them in memory only, DECISIONS D-67) |
| `--replay <dir>` | Every HTTP request is answered from `<dir>/replay.json` (format: `src/ui_logic/include/wizz/ui/replay_transport.hpp`) |
| `--fixed-now 2026-11-02T10:00:00Z` | Fixed clock, local zone UTC |
| `--no-pacing` | No 1 s spacing between Wizz requests |
| `--warm-cache` | Runs the outbound search once first, so the script's search is a cached replay (every fare reused, D-66) |
| `--cancel-after-ms <n>` | Presses Esc and Cancel n ms after the outbound search starts fetching |
| `--diagnostics` | Writes `diagnostics.json` (40 ms timer gaps, UI-loop watchdog, start delay, cancel record, final grids, DPI, layout checks) |

A run with `--autotest` or `--replay` on its command line never checks GitHub for updates (DECISIONS
D-45) and does not fetch the exchange rates at start (D-69), so self-tests stay offline and their request
counts unchanged.

Example (an isolated profile and a replayed network):

```powershell
.\WizzairHelper.exe --autotest out --data-root profile --replay tests\replay\named_destination `
    --fixed-now 2026-11-02T10:00:00Z --diagnostics --from Budapest --from-radius 200 `
    --to London --to-radius 30 --out 2026-11-10:2026-11-20 --ret 2026-11-12:2026-11-20 --then-currency HUF
```

The checker runs a named scenario and validates everything; `--scale 1.5` / `--scale 2` run it at
150 % / 200 % (`QT_SCALE_FACTOR`), `--window 980x600` starts the window at that size (through the
isolated profile's settings.json), `--theme auto|dark|light` saves that Theme choice there
(DECISIONS D-75), `--color-scheme light|dark`, `--high-contrast`,
`--fonts windows10` and `--text-rendering hinted|natural|natural-heavier` set the look's switches
below, and `--exe` can point at a packaged exe:

```powershell
python -I tests\gui\check_autotest.py --exe build\msvc-x64\src\app\Release\WizzairHelper.exe `
    --replay-root tests\replay --scenario heavy_cached --work-dir C:\Temp\autotest\heavy --scale 2
```

Scenarios: `named_destination`, `anywhere_return_from_top`, `not_served`, `same_area`, `over_500`,
`cancel_waiting`, `cancel_retrying`, the generated heavy replays `heavy_cached` and
`cancel_cached` (every route from Budapest in the frozen live map, about 6500 fares), and
`restart_shifted`: two runs on one `--data-root` profile, the second one day later and 20 minutes
after the first, which must ask Wizz Air only for each route's new last day, take the rest from
`fares.json`, say so in the Log and rewrite the file (DECISIONS D-66, D-67).

### The look's diagnostic switches

The app follows Windows: light or dark from the app mode (while the Settings window's Theme switch
is on Auto; Dark or Light keep that look, D-75), High Contrast with the system's colours
and the plain native style, and it changes along while it runs (DECISIONS D-53..D-56). Its text is
drawn natural and slightly heavier (D-58). For tests, screenshots and diagnosis these environment
variables override what Windows says and how text is drawn, like `QT_SCALE_FACTOR`; a normal
installation never needs them:

| Variable | Effect |
|---|---|
| `WIZZ_COLOR_SCHEME=light` / `dark` | Forces the colour scheme (the title bar follows), also over the Theme switch of the Settings window (DECISIONS D-75) |
| `WIZZ_HIGH_CONTRAST=1` / `0` | Says that High Contrast is on / off instead of asking Windows |
| `WIZZ_FONTS=windows10` | Leaves out the Windows 11 fonts (Segoe UI Variable, Cascadia Mono, Segoe Fluent Icons), as on a Windows 10 without Windows Terminal |
| `WIZZ_TEXT_RENDERING=hinted` / `natural` | Draws all text, popups and dialogs included, another way than the default, `natural-heavier` (ClearType without hinting, Segoe UI Variable and Cascadia Mono 50 units heavier on their weight axis; D-58): `hinted` as 2.0.0 did (Qt's default hinting, at 100 % DirectWrite GDI-classic ClearType: glyphs snapped to whole pixels), for comparisons and bug reports: 2.0.0's text, and at 100 % its layout too (at 150 % and 200 % lines are measured as laid out, so the window's lower part can sit 2 px lower than in 2.0.0); `natural` without the extra weight. Read when a font is built, so set it before the app starts; unknown values mean the default. `diagnostics.json` reports the rendering, the UI font's hinting and its weight axis (`environment.textRendering`, `fontHinting`, `fontWeightAxis`) and the face and weight axis of the UI, semibold, caption and monospace fonts (`environment.fonts`) |

The GUI test binary reads them too: `WIZZ_COLOR_SCHEME=dark` runs the whole suite in the dark look
(its tests compare with the theme's tokens, not with fixed colours), `WIZZ_FONTS=windows10` with the
Windows 10 families, `WIZZ_TEXT_RENDERING=hinted` with 2.0.0's text and metrics. Tests that change the look
(`LookGuard`) run with the defaults and restore these variables afterwards.

To compare a rendering in the built app without touching your own profile, run it as an autotest
(`--data-root` counts only with `--autotest`) on a replay and keep the window open afterwards. The
`heavy_cached` check writes a replay with the whole frozen route map (189 airports in the place
lists) into its work directory:

```powershell
python -I tests\gui\check_autotest.py --exe build\msvc-x64\src\app\Release\WizzairHelper.exe `
    --replay-root tests\replay --scenario heavy_cached --work-dir C:\Temp\autotest\heavy
$env:WIZZ_TEXT_RENDERING = 'hinted'
build\msvc-x64\src\app\Release\WizzairHelper.exe --autotest C:\Temp\wizz-hinted\out `
    --data-root C:\Temp\wizz-hinted\data --replay C:\Temp\autotest\heavy\replay `
    --fixed-now 2026-11-02T10:00:00Z --from Budapest --from-radius 0 --out 2026-11-03:2026-12-14 --keep-open
```
