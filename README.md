# WizzairHelper

A Windows desktop app that finds the cheapest Wizz Air trips from every Wizz airport near where you
start, to every airport near where you want to go (or anywhere), over a date range — then searches
the way back and pairs both legs into the cheapest round trips for your stay. Fares stream in while
the search runs, prices are converted to one currency, and double-clicking a fare opens it on
wizzair.com.

Native C++23 / Qt 6 Widgets / libcurl, built with CMake and vcpkg (MSVC x64).

## Repository map

| Path | What |
|---|---|
| `00-README.md` … `07-formatting-and-invariants.md`, `protocol-reference.md` | The specification (authority) |
| `fixtures/`, `history/` | Byte-exact test inputs and the dated evidence (frozen live dataset) |
| `src/core` | Pure logic: decimal, JSON, Unicode/locale, time, geo, catalog, planner, pricing, round trips |
| `src/services` | I/O: libcurl transport, caches, Wizz client, currency service, place resolver, search service |
| `src/ui_logic` | Qt-free presentation rules: texts, busy-panel model, date rules, grid rules, settings, autotest |
| `src/app` | The Qt 6 desktop application (`WizzairHelper.exe`) |
| `tests/` | Conformance suite (05), ui_logic tests, widget tests (`tests/gui`) and end-to-end autotest replays |
| `docs/` | [Architecture](docs/ARCHITECTURE.md), [decisions & deviations](docs/DECISIONS.md), [GUI design](docs/GUI_DESIGN.md), [release checks](docs/RELEASE_CHECKS.md) |

## Build

See [BUILDING.md](BUILDING.md). In short:

```powershell
git submodule update --init
cmake --workflow --preset full          # configure, build and test everything (Debug)
cmake --workflow --preset release-package   # release ZIP under build/msvc-x64-release/package
```

Settings, caches and `errors.log` live in `%LOCALAPPDATA%\WizzairHelper\`.

Fares come from wizzair.com's unofficial website API; geocoding uses Photon / Nominatim
(data © OpenStreetMap contributors); exchange rates come from Frankfurter / currency-api.
Keep usage personal and low-volume.
