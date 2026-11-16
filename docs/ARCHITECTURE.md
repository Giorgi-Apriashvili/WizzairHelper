# WizzairHelper — architecture and conventions

The specification (chapters `00`–`07` and the protocol reference, kept locally in `spec/` and not
published with the code; code comments cite it as "01 §10.3" etc.) together with `fixtures/` is the
authority. This file records how the C++ code base is organised so that every
contributor builds the same thing. Deliberate deviations from the spec are listed in
[`DECISIONS.md`](DECISIONS.md).

## Toolchain

- **C++23** (the user's choice; it overrides the C++20 mentioned in 06), MSVC x64 (VS 2026), CMake ≥ 3.30.
- Dependencies come from the pinned **vcpkg** submodule (`external/vcpkg`, tag `2026.10.16`) through
  `vcpkg.json`: libcurl (Schannel + brotli), qtbase 6.11.2 (feature `gui`), GoogleTest (feature `tests`).
- ICU is the Windows system ICU (`icu.dll`, `<icu.h>` / `icu.lib` from the Windows SDK) — 02 §4.3.

## Build

```powershell
cmake --workflow --preset core             # core + services + tests, no Qt
cmake --workflow --preset full             # everything incl. WizzairHelper.exe
cmake --workflow --preset release-package  # Release ZIP + installer in build/msvc-x64/dist
```

[`BUILDING.md`](../BUILDING.md) has the prerequisites, the presets, the tests, packaging and the
`--autotest` / replay usage; [`RELEASING.md`](RELEASING.md) the CI workflow and the release process.

Presets use the "Visual Studio 18 2026" generator, so no developer shell is needed. Build trees live
in `build/<preset>`. In a git worktree without the vcpkg submodule, add
`-DCMAKE_TOOLCHAIN_FILE=<main checkout>/external/vcpkg/scripts/buildsystems/vcpkg.cmake`; to reuse an
already-installed dependency tree without running vcpkg, also add
`-DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR=<main checkout>/build/vcpkg_prefetch`.

Sources are collected with `CONFIGURE_DEPENDS` globs: adding a `.cpp`/`.hpp` under a target's
directory needs no CMake edit.

## Targets and layering (06 §1)

| Target | Directory | Owns | May depend on |
|---|---|---|---|
| `wizzairhelper_core` (`wizz::core`) | `src/core` | Pure logic, no I/O: time, decimal, JSON, Unicode text, locale/formatting, geo, domain values, airport catalog + offline place matching, planner, pricing/FX tables, result views, booking links, round trips; abstract `Clock`, `http::Transport`, cancellation, progress records | C++ std, Windows ICU |
| `wizzairhelper_services` (`wizz::services`) | `src/services` | Everything that performs I/O or owns threads: libcurl transport, file cache + AppPaths, request gate, Wizz client, currency service, geocoder + place resolver, flight search service, system clock, self-updater core (GitHub release check, verified downloads, update schedule, silent installer command line — D-40..D-44) | core, libcurl, Win32 (incl. CNG) |
| `wizzairhelper_ui_logic` (`wizz::ui_logic`) | `src/ui_logic` | Qt-free presenter / view-model logic of the desktop app (05 §7.2), namespace `wizz::ui`: string catalogue, leg status / log texts, BusyPanel state machine, date and nights coupling, request building, settings.json, `--autotest` options + replay transport (D-30), summary.json, grid models (sort, selection, streaming), the self-updater's rules and texts (`UpdateModel`, D-45..D-49) | core, services |
| `WizzairHelper` | `src/app` | Qt 6 Widgets UI (thin widgets over `ui_logic`), diagnostics, crash reporting, `--autotest` driver, the update bar and the updater's lifecycle (worker checks, installer start on exit, AppMutex); the exe's resources and the application icon in `src/app/resources` (below) | core, services, ui_logic, Qt |
| `wizzairhelper_tests` | `tests` | GoogleTest conformance suite: fake transport, fixed clock, recording delay, fixture replay | core, services, ui_logic, gtest |

Core, services and ui_logic never include Qt. Nothing in core performs I/O.

Header layout: public headers under `src/<target>/include/wizz/<target>/…`, included as
`#include "wizz/core/decimal.hpp"`. Private helpers live next to the `.cpp` files in `src/<target>/src/`.

## Resources of the app

| File | What |
|---|---|
| `src/app/resources/WizzairHelper.rc` | The exe's version resource (D-37) and its icon `IDI_ICON1` from `icon/WizzairHelper.ico` (D-51) |
| `src/app/resources/WizzairHelper.manifest` | Application manifest: per-monitor-v2 DPI awareness (01 §3, 06 §4) |
| `src/app/resources/icon/` | The application icon (D-51): two SVG sources, the PNG of every size and the `.ico`, all committed; `tools/make_app_icon.py` regenerates the outputs. The ten PNGs are also the Qt resource `:/icons/` of `wizzairhelper_app`, the window icon (`application_icon()` in `app_info.hpp`) |

The build needs no image tools: it compiles the committed `.ico` and PNGs as they are.

## The look (DECISIONS D-53..D-57)

The app draws its own look instead of the WinForms one of 01; these files hold it:

| File | What |
|---|---|
| `src/app/lib/ui_style.hpp/.cpp` | `style::Theme`: the light, dark and High Contrast colour tokens; the fonts with their Windows 10 fallbacks; icon glyphs painted from their outlines; choosing the look (High Contrast, `WIZZ_COLOR_SCHEME`, the Windows app mode), installing it, following Windows' changes (`refresh_appearance`, `ThemeNotifier`) |
| `src/app/lib/modern_style.hpp/.cpp` | `ModernStyle`, the `QProxyStyle` over Fusion that paints inputs, buttons, check boxes, tabs, headers, scroll bars, list and menu items, popups, tooltips and focus rings from the tokens; `control_height` |
| `src/app/lib/section_card.hpp/.cpp` | `SectionCard` (a titled input card) and `GlyphLabel` (one icon glyph) |
| `src/app/lib/trailing_row_layout.hpp/.cpp` | The card row that keeps a search button at its right end and moves it under the row when it does not fit |
| `src/app/lib/panel_progress_bar.hpp/.cpp` | The busy panel's self-painted bar, still while idle (D-34) |

Widgets that copy a token into a palette, brush or text format connect to `style::theme_notifier()`
(`style::on_theme_change`, `style::set_text_tone`); custom-painted widgets read `style::theme()` when
they paint. No style sheets.

Their tests in `tests/gui`: `look_test.cpp` (choice of the look, live switch, High Contrast, fonts,
WCAG contrast), `modern_style_test.cpp`, `trailing_row_layout_test.cpp`, the widget tests of each
control, and `look_snapshot_test.cpp` (renders every surface for a review; manual). Test support:
`look_test_support.hpp` (the suite's look and `LookGuard`) and `minimum_window.hpp` (the client areas
of the minimum and default windows).

## Code conventions

- Namespace `wizz` (sub-namespaces only where they add clarity, e.g. `wizz::http`).
- Types `PascalCase`; functions, methods, variables `snake_case`; constants `kPascalCase`;
  private data members `name_`; files `snake_case.hpp/.cpp`. Qt-facing code in `src/app` follows Qt
  naming for overrides, signals and slots.
- Strings are UTF-8 `std::string` everywhere; convert to UTF-16 only at the ICU / Win32 / Qt boundary.
  Where the spec counts lengths in UTF-16 code units (02 §4.1, §5.7, §5.8), compute them on UTF-16.
- Money and FX values are `wizz::Decimal` (exact System.Decimal emulation). Never pass money through
  `double` (06 §3, 03 §7.1). JSON numbers are parsed from their lexeme.
- Value types are immutable after construction; copy and modify to derive. Results that the UI must
  identify by identity (explicit pick) are held as `std::shared_ptr<const T>`.
- Errors that the spec models as exception types are C++ exceptions with the same hierarchy and catch
  order (04 §10). `OperationCancelled` never derives from an API error. Use `std::optional` /
  `std::expected` for local, expected parse failures.
- RAII for every handle (curl, files, COM, threads). No naked `new`/`delete`.
- No global mutable state except the documented caches inside services.
- `const` and `[[nodiscard]]` by default; `noexcept` where it is true; `std::span`/`std::string_view`
  for non-owning parameters; `enum class`; ranges algorithms where they read better.
- Warnings are errors (`/W4 /WX /permissive-`). Do not silence a warning without a comment.

## Threading model (01 §14, 03 §5.2, 06 §2)

- Searches always run on a worker thread, even when everything is cached. The entry point returns a
  future before any progress callback runs.
- Engine callbacks (`NoticeSink`, `ProgressSink`, partial-result sink) run synchronously on the worker
  thread. The UI marshals them into its own event queue tagged with the job ID and drops stale ones.
- One `std::stop_token` per job flows through every wait: locks, the request gate, back-off delays,
  HTTP transfers and publication. User cancellation is distinct from per-attempt timeouts.

## Tests

- Test names follow the spec IDs: `TEST(Catalog, CT_CAT_01_Parses_map_and_trims_names)`.
- Fixtures are read from `<test-binary-dir>/Fixtures/<name>` (copied post-build, byte-exact).
- Tests never touch live hosts or the user profile; caches use temporary directories.
- The suite runs under en-US with a fixed clock (2026-11-02T10:00:00Z, local zone UTC) unless a case
  says SYSTEM.
- GUI tests run under the app's own look (light, High Contrast off) on the minimal Qt platform
  with real fonts (`QT_QPA_PLATFORM=minimal:enable_fonts`), so layout tests measure what Windows
  shows; tests that change the look use `LookGuard` (DECISIONS D-57).
- Tests hold on a busy CI runner (4 shared vCPUs, 8 test processes at once, Windows Server):
  wait for the condition itself (`wizz::test::pump_until`, `gui::wait_until`, capped at
  `kWaitCap`; a cap costs nothing when the condition holds), never a fixed sleep as a
  precondition; measure what the code controls (counts, the time the test measured around the
  code) or bound by the code's or the scenario's own constants; never depend on desktop settings
  such as client-area animations, which Windows Server turns off.
