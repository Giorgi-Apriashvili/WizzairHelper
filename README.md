<img src="src/app/resources/icon/WizzairHelper-128.png" width="128" height="128" alt="WizzairHelper icon: a magenta map pin with a white plane, and an orange ring around its tip">

# WizzairHelper

[![build](https://github.com/Giorgi-Apriashvili/WizzairHelper/actions/workflows/build.yml/badge.svg)](https://github.com/Giorgi-Apriashvili/WizzairHelper/actions/workflows/build.yml)

A Windows desktop app that finds the cheapest Wizz Air trips from every Wizz airport near where you
start, to every airport near where you want to go (or anywhere), over a date range — then searches
the way back and pairs both legs into the cheapest round trips for your stay. Fares stream in while
the search runs, prices are converted to one currency, and double-clicking a fare opens it on
wizzair.com.

WizzairHelper is an independent project. It is not affiliated with, endorsed by or connected to
Wizz Air; its icon is original artwork, not an airline logo.

It follows Windows' light or dark app mode, also when you switch it while the app runs, and
Windows' High Contrast themes.

Native C++23 / Qt 6 Widgets / libcurl, built with CMake and vcpkg (MSVC x64).

## Download / install

Windows 10 (1903 or later) or Windows 11, x64. Download from the
[Releases page](https://github.com/Giorgi-Apriashvili/WizzairHelper/releases):

- `WizzairHelper-X.Y.Z-win64-setup.exe`: per-user installer, no administrator rights. It installs
  into `%LOCALAPPDATA%\Programs\WizzairHelper`, adds a **WizzairHelper** Start-menu entry (a desktop
  icon if you tick it) and an uninstaller (Settings > Apps).
- `WizzairHelper-X.Y.Z-win64.zip`: the same files without an installer. Unpack the folder anywhere
  and start `WizzairHelper.exe`; nothing else needs to be installed.
- `SHA256SUMS.txt`: the SHA-256 of both, to check a download
  (`Get-FileHash <file> -Algorithm SHA256`).

The installer is not code-signed, so SmartScreen may show "Windows protected your PC" the first
time; choose "More info", then "Run anyway". `WizzairHelper.exe --version` prints the version.

## Repository map

The code follows a detailed specification that is not published here; comments cite its chapters
as "01 §10.3" and so on, and [`docs/DECISIONS.md`](docs/DECISIONS.md) records how conflicts in it
were resolved.

| Path | What |
|---|---|
| `fixtures/`, `history/live-cache-2026-11-02/` | Byte-exact test inputs: captured responses and a frozen live dataset |
| `src/core` | Pure logic: decimal, JSON, Unicode/locale, time, geo, catalog, planner, pricing, round trips |
| `src/services` | I/O: libcurl transport, caches, Wizz client, currency service, place resolver, search service, self-updater core |
| `src/ui_logic` | Qt-free presentation rules: texts, busy-panel model, date rules, grid rules, settings, autotest |
| `src/app` | The Qt 6 desktop application (`WizzairHelper.exe`); its icon's sources and images in `src/app/resources/icon` |
| `tests/` | Conformance suite (05), ui_logic tests, widget tests (`tests/gui`) and end-to-end autotest replays |
| `packaging/` | The package's README and notices, the Inno Setup installer script |
| `tools/` | Release helpers: wait for CI, wait for a tag's run, check a published release; the icon generator (`make_app_icon.py`) |
| `docs/` | [Architecture](docs/ARCHITECTURE.md), [decisions & deviations](docs/DECISIONS.md), [GUI design](docs/GUI_DESIGN.md), [release checks](docs/RELEASE_CHECKS.md), [releasing](docs/RELEASING.md) |

## Build

See [BUILDING.md](BUILDING.md). In short:

```powershell
git submodule update --init
cmake --workflow --preset full          # configure, build and test everything (Debug)
cmake --workflow --preset release-package   # release ZIP + installer under build/msvc-x64/dist
```

Releases are cut by pushing a version tag; see [docs/RELEASING.md](docs/RELEASING.md).

Settings, caches and `errors.log` live in `%LOCALAPPDATA%\WizzairHelper\`; the updater keeps its
state, log and downloads in its `updates\` subfolder.

Fares come from wizzair.com's unofficial website API; geocoding uses Photon / Nominatim
(data © OpenStreetMap contributors); exchange rates come from Frankfurter / currency-api.
Keep usage personal and low-volume.

## Updates

Once a day WizzairHelper asks `api.github.com` for the latest release of this repository (one
request, no personal data; an hour later if it failed). When there is a newer version, the
installed app downloads the installer from GitHub, checks it against the release's
`SHA256SUMS.txt` and installs it silently when you close the app — or at once with "Restart and
update" in the bar under the search inputs. A portable copy (the ZIP) only shows a link to the
release page. Details: [DECISIONS D-40..D-49](docs/DECISIONS.md#self-updater).

## License

MIT — see [LICENSE](LICENSE) (`LICENSE.txt` in the release package). The package also ships the
licences of its third-party components (Qt under LGPL-3.0, libcurl, zlib, brotli, …) in
`THIRD-PARTY-NOTICES.txt` and `licenses/`.
