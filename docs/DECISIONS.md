# WizzairHelper — decisions and deliberate deviations

Authority (00-README): 07 applies across the package; each numbered chapter is authoritative for its
subsystem; 05 holds the acceptance inputs/outputs. Where the documents disagree, the resolutions
below are binding for this code base. Each entry names both sides.

## Toolchain and structure

| # | Decision | Reason |
|---|---|---|
| D-01 | **C++23**, not the C++20 named in 06 §1. | User decision (2026-11-15). |
| D-02 | All third-party dependencies via the pinned vcpkg submodule, **including Qt** (qtbase 6.11.2); libcurl with Schannel + brotli; GoogleTest. ICU is the Windows system `icu.dll` (02 §4.3), not a vcpkg port. | User decision; ICU is an OS component and matches .NET-on-ICU behaviour. |
| D-03 | `wizzairhelper_core` is pure (no I/O, no threads). The place resolver's geocoding and the flight search service live in `wizzairhelper_services` because they perform I/O / own worker threads. The offline place matching (catalog) stays in core. | 06 §1 lists place resolution and the engine under core, but also requires core to have only abstract I/O interfaces; this split keeps core pure and both libraries are test-drivable. |
| D-04 | Qt 6 has no `QWinTaskbarProgress`; taskbar progress uses `ITaskbarList3` COM directly (01 §10.9). Qt's progress bar does not lag like the native control, so the "jump past, then back" trick (01 §10.6) is kept only where harmless. Win32 calls in 01 describe required *behaviour*. | 06 mandates Qt Widgets; 01 describes the predecessor's Win32 mechanics. |

## Spec conflicts resolved

| # | Topic | Side A | Side B | Resolution |
|---|---|---|---|---|
| D-10 | Duplicate IATA in the map | 02:471 "Find returns the last one, All/_real keep both" | 02 §5.2 pseudocode, 07 §4, CT-LATENT-02 | De-duplicate in the catalog constructor (OrdinalIgnoreCase): keep the **first occurrence's position**, use the **last occurrence's value** (all fields). Counts unchanged; the planner sees unique IATAs. |
| D-11 | Esc vs Cancel | 01 §10.4 "Esc cancels without changing the text" | 01 §6.3, 06 §2, 07 §4 | One idempotent handler: disable Cancel, show `Cancelling… (finishing the current request)`, signal once; ignore repeats until a new cancellable job begins. |
| D-12 | Round-trip heap ties at the limit | 05 CT-RT-02 [N] "implementation-defined, first-come" | 07 §3 deterministic full key | Full 07 §3 key (total, outbound date, nights, outbound departure, return departure, out from/to, ret from/to IATA, generation sequence) for admission, eviction and the final sort. Reproduces CT-RT-02. |
| D-13 | FX table validation (CT-LATENT-01) | 03 §7.2 per-row skips | 07 §4 "valid JSON with missing or wrongly typed fields → reject that provider's table" | Structural problems reject the **whole** table and the next source is tried: non-array root (Frankfurter) / non-object `eur` (Fawaz), a non-object element, a missing or non-string `quote`, a missing or non-number `rate`, a number that does not fit a decimal. Row-level skips stay for: `base` missing or not EUR, empty quote, rate ≤ 0 (Frankfurter); key not 3 letters, value ≤ 0 (Fawaz). Non-numeric Fawaz values reject the table. |
| D-14 | Next-departure merge | 03 §4.3.10 prose "pseudo-station connections included" | 03 §4.2 pseudocode and §11 walk-through | Merge `Next` only over connections that survive the mirror, destination and home-area filters (pseudocode). |
| D-15 | `api-url.txt` strict regex | 04 §7.4 (empty code block) | protocol-reference §3 | Full-string match of the trimmed file against `^https://be\.wizzair\.com/\d+(\.\d+)+/Api$` (ASCII digits). |
| D-16 | `InvalidFromDate` shifts | protocol-reference §5/§6 "retry once" | 04 §9.2 code: up to two shifts | 04 wins: original + up to 2 shifts (3 sends), P-02. |
| D-17 | `FormatDuration` rounding | 07 §2 labels round half away from zero | 01 §11.10 banker's rounding; 07 exempts values "specified as nearest-even" | `FormatDuration` under 60 s uses round-half-to-even (`2.5 s → "2 s"`). Distance/duration **labels** (`{x:0}`, `{x:0.0}`) use the 07 §2 label algorithm. |
| D-19 | `MatchLegs` with an empty leg | 03 §10.2 prose: "if either leg has zero flights, there is no problem" | 03 §10.2 pseudocode checks each leg separately | Pseudocode: F4 is reported when the outbound has fares landing outside the return's origin area even if the return is empty (and F5 symmetrically). |
| D-18 | Test count | 05 heading "112 cases" | 05 intro / 06 §5 "114 baseline" | 112 catalogue cases + CT-LATENT-01/02 = 114. The 05 §7 scenarios (P-xx, U-xx, R-xx) are implemented as additional tests. |

## Pathological inputs (spec allows a choice; recorded here as 02/04 request)

| # | Case | Decision |
|---|---|---|
| D-20 | Unknown response charset (04 §2.2, "gap G1") | Decode as UTF-8 with replacement instead of failing. |
| D-21 | HTTP version | Force HTTP/1.1 (`CURL_HTTP_VERSION_1_1`), as .NET does. |
| D-22 | Timetable / map date-times carrying `Z` or an offset (02 §2.4, 04 §9.4) | Keep the wall-clock value and ignore the offset (Wizz never sends offsets). Accepted forms: `yyyy-MM-dd`, `yyyy-MM-ddTHH:mm[:ss[.fffffff]]` (also with a space instead of `T`), optional surrounding white space, and invariant `MM/dd/yyyy`. Anything else is unparsable. |
| D-23 | `geocode.json` entry whose value is `null` or not an array (02 §6.9) | Treat as a cache miss (the .NET predecessor crashed with a NullReferenceException). |
| D-24 | Frozen live dataset `history/live-cache-2026-11-02/` | Supplied by the user and committed byte-exact (hashes match 02 §14). Tests read it from `<test-binary-dir>/LiveCache/`. The 114 baseline cases use only `fixtures/`; the live-map "[verified]" tables of 02/03 are added as extra tests on top. |

## Naming and emulation notes

| # | Topic | Decision |
|---|---|---|
| D-25 | `AirportCatalog.Near` | Exposed as `airports_near(point, radius_km)`: `<windows.h>` defines `near` as an empty macro, which breaks any member named `near`. |
| D-26 | Hand-written replacements for the .NET regexes (`CityNameOfFake`, `TrailingCodeRegex`, `ExtractApiUrl`) | Mirror .NET semantics exactly (verified against .NET 10): `\s` = `char.IsWhiteSpace`, `\d` = any BMP decimal digit, `\b` word characters = L*, Mn, Nd, Pc, U+200C/U+200D per UTF-16 unit, IgnoreCase = en-US case equivalences. No `std::regex`. |
| D-27 | Amount that does not fit a decimal in a timetable | `json::FormatError` with the .NET text "One of the identified items was in an invalid format."; the client wraps any `json::Error` as `Wizz Air sent a timetable that could not be read: …` (04 §9.2). |
| D-28 | Unpaired surrogate escapes in JSON strings (`"\ud800"`, `"\udc00"`) | As on .NET 10 (verified): `JsonDocument.Parse` accepts them (the RFC 8259 grammar does), only reading that text throws `InvalidOperationException`. `json::parse` keeps such a string as an unreadable value of kind String: `as_string`, `get_string` and `Member::name` throw `json::TypeError` with the .NET text — `Cannot read incomplete UTF-16 JSON text as string with missing low surrogate.` for a high surrogate not followed by another `\u` escape; `Cannot read invalid UTF-16 JSON text as string. Invalid surrogate value: '0x…'.` (format `X2`) naming a leading low surrogate, or the escape after a high surrogate when that is not a low one. The first problem in a string wins; everything else in the document stays readable, so junk in a field nobody reads no longer rejects the route map (02 §5.1) or a Photon answer (02 §6.4, §6.6). `find` / `get_string` scan the members from the last to the first (last duplicate wins) and throw when they reach a name holding an unpaired surrogate before a match; .NET's `TryGetProperty` throws there only when the escaped name is longer than the requested one and agrees with it up to its first backslash. Consumers keep their .NET catch clauses (e.g. `ParseValidationCodes` catches only `JsonException`, so the TypeError propagates); where .NET reads through `JsonSerializer` (settings.json, geocode.json), which wraps it in a `JsonException`, it is handled as one — unknown properties are skipped unread there. Malformed or truncated escapes and raw UTF-8 surrogates stay `ParseError`s; the writer is unchanged. |

## Planned test-only extensions

| # | Extension | Reason |
|---|---|---|
| D-30 | Hidden `--autotest` companions, approved by the user (2026-11-15), parsed by `wizz::ui::AutoTestOptions` (`src/ui_logic`) and honoured only together with `--autotest`, with the 01 §17.1 matching rules (exact, case-sensitive, first occurrence wins, value = next argument): **`--data-root <dir>`** replaces `%LOCALAPPDATA%\WizzairHelper` as the root of `settings.json`, `errors.log` and `cache\` (isolated profile; `app_paths` takes it as the root override). **`--replay <dir>`** serves every Wizz, FX and geocoder request through `wizz::ui::ReplayTransport` from `<dir>/replay.json` plus the body files it names: ordered rules that match on `method`, `url` (exact), `urlPrefix`, `urlContains`, `bodyContains` and answer with `status` (200), `file` or `body`, `contentType` (by extension; `null` = none), `headers`, `delayMs` (cancellable), `error` (`transport` / `timeout`, with `message`) and `times` (use count); the first matching rule wins; an unmatched request gets 404 `{"message":"No HTTP resource was found"}`; unknown fields are rejected at start-up (full format in `replay_transport.hpp`). **`--fixed-now <yyyy-MM-ddTHH:mm:ss[.fffffff][Z]>`** runs on a `FixedClock` at that UTC instant with the local zone UTC. Relative directories are made absolute against the current directory; a bad `--fixed-now` throws a `FormatError` before the window opens, like a bad `--out` date. | 05 §5 requires unattended GUI checks on an isolated profile and CI "uses fixtures only", which the documented flags alone cannot provide. Documented flags and `summary.json` stay unchanged. |
