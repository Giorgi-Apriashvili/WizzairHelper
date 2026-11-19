<img src="src/app/resources/icon/WizzairHelper-128.png" width="128" height="128" alt="WizzairHelper icon: a magenta map pin with a white plane, and an orange ring around its tip">

# WizzairHelper

[![build](https://github.com/Giorgi-Apriashvili/WizzairHelper/actions/workflows/build.yml/badge.svg)](https://github.com/Giorgi-Apriashvili/WizzairHelper/actions/workflows/build.yml)

A Windows desktop app that finds the cheapest Wizz Air trips from every Wizz airport near where you
start, to every airport near where you want to go (or anywhere), over a date range — then searches
the way back and pairs both legs into the cheapest round trips for your stay. Fares stream in while
the search runs, prices are converted to one currency, and double-clicking a fare opens it on
wizzair.com. **Homes on Airbnb** (a button above each results table, also in the right-click menu)
opens Airbnb's own search in your browser for the homes around the destination city's centre, for
the stay dates of the selected row.

WizzairHelper is an independent project. It is not affiliated with, endorsed by or connected to
Wizz Air or Airbnb; its icon is original artwork, not an airline logo.

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
state, log and downloads in its `updates\` subfolder. Fares fetched from Wizz Air are kept in
`cache\fares.json` for up to 6 hours: a search asks Wizz Air only for the days it did not fetch
within the time chosen under **Settings > Searches > Reuse fares fetched within** (30 minutes
unless you change it; fares for today at most 30 minutes), and the Log says how old the reused
fares were.

**Homes on Airbnb** asks Photon / Nominatim (cached like any place) where the destination city's
centre is, then opens `airbnb.com` in your default browser on the area within **Settings >
Searches > Homes on Airbnb within** of it (5 km unless you change it), for the selected row's stay
dates and adults. The app itself never contacts Airbnb, and Airbnb shows its own current prices.
Islands and regions without one city (Santorini, Gran Canaria, Malta) open Airbnb's search for
the name instead. An outbound row stays the minimum nights of "stay N to M nights" from its date,
a return row as many nights before it, and a round trip from the outbound to the return.

Fares come from wizzair.com's unofficial website API; geocoding uses Photon / Nominatim
(data © OpenStreetMap contributors); exchange rates come from Frankfurter / currency-api.
Keep usage personal and low-volume.

## Updates

Once a day WizzairHelper asks `api.github.com` for the latest release of this repository (one
request, no personal data; an hour later if it failed). When there is a newer version, the
installed app downloads the installer from GitHub, checks it against the release's
`SHA256SUMS.txt` and installs it silently when you close the app — or at once with "Restart and
update" in the bar under the search inputs. A portable copy (the ZIP) only shows a link to the
release page.

An update you put off (with "Later", with "only tell me", or by closing the app when it was to
ask you) is offered again in that bar every time you start the app, from what the app already
knows: no extra request to GitHub, so it still asks GitHub only once per chosen period. An
installer it downloaded earlier is offered as ready only while its bytes still match the checksum
they had when they were verified. "Later" hides the bar until the next start (with the default
choice the update is still installed when you close the app).

To change this, click **Settings** (the gear at the right end of the Outbound / Return / Round
trips / Log tabs). Its "Updates" section shows your version, when it last checked (after a
check that failed: when it last checked successfully) and what it found, and has:

- **Check now**, which checks at once, also when automatic checks are off; when an update is
  ready or available, its button (**Restart and update**, **Update now** or, for a portable copy,
  **Download**) appears right under Check now, so you need not close the window to use the bar;
- **Check for updates automatically** with **Once a day**, **Once a week** or **Once a month**
  (the first time you open the app on a new day, in a new week starting on Monday, or in a new
  month); clear the box and the app contacts GitHub only when you press Check now, or "Update now"
  in the bar;
- **When an update is found:** download it and install it when you close the app (the default),
  download it and then ask before installing ("Restart and update" in the bar; closing the app
  leaves it), or only say that it is available ("Update now" in the bar downloads it, then
  restarts the app into the installer).

Changes apply at once and are kept in `settings.json`. A portable copy shows the install choices
greyed out, since it can only point you to the release page.
Details: [DECISIONS D-40..D-49, D-59..D-63, D-74](docs/DECISIONS.md#self-updater).

## License

MIT — see [LICENSE](LICENSE) (`LICENSE.txt` in the release package). The package also ships the
licences of its third-party components (Qt under LGPL-3.0, libcurl, zlib, brotli, …) in
`THIRD-PARTY-NOTICES.txt` and `licenses/`.
