# Releasing WizzairHelper

How WizzairHelper is built, tested and shipped on GitHub
(<https://github.com/Giorgi-Apriashvili/WizzairHelper>, branch `main`). Decisions D-35 – D-39 in
[`DECISIONS.md`](DECISIONS.md) record the details; [`RELEASE_CHECKS.md`](RELEASE_CHECKS.md) is the
release record.

## Principles

- **`main` is always releasable.** Every finished change is committed and pushed on its own, and CI
  builds and tests every push.
- **One source of truth for the version:** `project(WizzairHelper VERSION X.Y.Z)` in
  `CMakeLists.txt`. The exe's version resource, `--version`, the file names and the installer take
  it from there; a tag that disagrees with it fails CI.
- **A tag cuts a release.** CI builds, tests, packages and publishes; nothing is uploaded by hand.
- **Release only when the owner says so.** Patch (X.Y.Z) for fixes, minor (X.Y.0) when behaviour
  changes.
- **Every download has a checksum.** `SHA256SUMS.txt` ships with every release, and anything that
  installs or updates the app checks against it.
- **A red CI is a finding, not noise.** Find the cause; never re-run until it happens to pass.
- **Commits, tags and notes are the owner's.** No AI or tool attribution lines.

## The pipeline

[`.github/workflows/build.yml`](../.github/workflows/build.yml) runs on pushes to `main`, `v*` tags
and pull requests, on the `windows-2025-vs2026` image (Visual Studio 2026, CMake 4, Inno Setup 6,
Python 3).

**Job `build`**, two legs with `fail-fast: false`, both configured with the `msvc-x64` preset:

| Leg | Build / test preset | Also |
|---|---|---|
| `msvc-release` | `release` | the `--autotest` end-to-end runs (`ctest -L autotest`), `dist`, `SHA256SUMS.txt`, the artifact `wizzairhelper-windows-x64` |
| `msvc-debug` | `debug` | |

1. Checkout with the `external/vcpkg` submodule.
2. **Tag guard** (tags only): the tag must be `v` + the project version.
3. **vcpkg binary cache** (`VCPKG_DEFAULT_BINARY_CACHE` + `actions/cache/restore` and `/save`,
   keyed by the submodule commit, `vcpkg.json` and the runner image). The first run builds Qt from
   source (Debug and Release) and takes well over an hour; later runs restore it. The save runs
   right after Configure, even when a later step fails: the plain `actions/cache` saves only on
   success, and the first run lost its Qt build to failing tests that way.
4. Configure, build and test through the same presets as local work:
   `ctest --preset <p> --output-on-failure -LE autotest --parallel <CPUs of the runner>`. The
   timing tests (label `timing`) run alone and hold on a busy runner (DECISIONS D-50).
5. The `autotest` label in `msvc-release` only: the real app window, one run at a time. The test
   preset fails when no test matches, so a runner without Python fails instead of skipping them.
6. `cmake --build --preset dist`, `SHA256SUMS.txt`, upload (`if-no-files-found: error`).

**Job `release`** (only `v*` tags, `needs: build`, `contents: write`): downloads the artifact and
creates the GitHub release with its three files (`softprops/action-gh-release@v2`). Its generated
notes are a placeholder that the written notes replace.

## Release assets

| File | What |
|---|---|
| `WizzairHelper-X.Y.Z-win64.zip` | Portable: one top-level folder `WizzairHelper-X.Y.Z-win64` with `WizzairHelper.exe`, its DLLs and plugins, `README.txt`, `THIRD-PARTY-NOTICES.txt`, `licenses/` |
| `WizzairHelper-X.Y.Z-win64-setup.exe` | Inno Setup installer of exactly the same files (`packaging/WizzairHelper.iss`) |
| `SHA256SUMS.txt` | `<lowercase sha256>  <file name>` per file, ASCII |

The installer and update contract (D-38):

- Per user, no administrator rights: `%LOCALAPPDATA%\Programs\WizzairHelper` by default, a
  Start-menu entry, an optional desktop icon, the uninstaller `unins000.exe` in the install folder
  (Settings > Apps). The settings and caches in `%LOCALAPPDATA%\WizzairHelper` stay on uninstall.
- An installed copy is recognised by `unins000.exe` next to `WizzairHelper.exe`; anything else is
  a portable (ZIP) copy.
- The app holds the mutex `WizzairHelper.AppMutex` while it runs (D-36); Setup and the uninstaller
  check it (`AppMutex`), and Setup closes programs that use its files (`CloseApplications=yes`).
- Silent update: `<setup.exe> /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS [/RELAUNCH]`,
  then the app exits. Setup waits up to 30 s for every running copy to exit; a copy still running
  after that ends Setup with exit code 1 and nothing changed. With `/RELAUNCH` Setup starts
  `WizzairHelper.exe` again after a successful install; without it nothing is started. An
  interactive install offers "Launch WizzairHelper" on its last page.
- `WizzairHelper.exe --version` prints `WizzairHelper X.Y.Z` (D-35). From PowerShell, pipe it to
  capture it: `$(& .\WizzairHelper.exe --version 2>&1 | ForEach-Object { "$_" })`.

## Day to day

- **For each finished change:**
  - build both configurations: `cmake --build --preset debug`, `cmake --build --preset release`;
  - run the full test suite: `ctest --preset debug -LE autotest`, `ctest --preset release` (with the
    `autotest` runs);
  - check the diff: `git diff --check`;
  - commit with a plain-English title and a body saying what changed and why;
  - push to `main`.
- **Don't sit waiting for CI.** Start `pwsh tools/ci_wait.ps1 -Sha (git rev-parse HEAD)` in the
  background and keep working. When it ends it prints each job's result, its minutes, and the step
  that failed.
- **When CI is red:**
  1. Read the failing assertion (`gh run view <id> --log-failed`).
  2. Reproduce it locally and fix the cause.
  3. A timing test that fails only on the runner (the `autotest` timer-gap limits, the in-process
     `R_03` checks) needs the owner's rules for tests that hold on a busy runner: wait for the
     condition itself, derive bounds from the code's constants, reproduce under load, and verify
     the fix with at least 30 loaded runs.

The helper scripts in `tools/` default to `-Repo Giorgi-Apriashvili/WizzairHelper` and take a
GitHub token from git's credential store (`git credential fill`), in memory only and never printed;
without one they make unauthenticated calls (60 an hour).

## Cutting a release

Before you start: the owner said "release", CI is green on HEAD (both legs), and the notes are
written.

1. Write the notes in a file outside the repository (`release-notes-X.Y.Z.md`); see
   [Release notes](#release-notes).
2. Bump the version in `CMakeLists.txt` only (`project(WizzairHelper VERSION X.Y.Z ...)`) and commit
   it as `Release X.Y.Z` (that one line, nothing else). Optionally check the artefacts first:
   `cmake --build --preset dist` writes both files to `build/msvc-x64/dist` (see
   [Checking the installer locally](#checking-the-installer-locally)).
3. Tag and push:
   ```
   git tag vX.Y.Z
   git push origin main
   git push origin vX.Y.Z
   ```
4. Wait for the tag's run: `pwsh tools/tag_wait.ps1 -Tag vX.Y.Z`. All three build legs and the
   release job must succeed.
5. Replace the generated notes:
   `gh release edit vX.Y.Z --notes-file release-notes-X.Y.Z.md`.
6. Verify the published release: `pwsh tools/release_check.ps1 -Tag vX.Y.Z -Exe WizzairHelper.exe`.
   It checks that:
   - the release is not a draft or prerelease;
   - the three assets are there, with sane sizes (about 11.5 MB for the ZIP, 10 MB for the
     installer);
   - every line of `SHA256SUMS.txt` matches its downloaded file;
   - the packaged `WizzairHelper.exe --version` reports `WizzairHelper X.Y.Z`, run with an isolated
     profile;
   - `releases/latest` points at the tag.

   It downloads into `tools/rel-vX.Y.Z` (ignored by git); delete that folder afterwards.
7. Record the release in [`RELEASE_CHECKS.md`](RELEASE_CHECKS.md): version, date, commit and one
   line of what it added.

**If the tag's run fails:**

- *Nothing was published* (the release job didn't run): delete the tag locally and on GitHub
  (`git tag -d vX.Y.Z`, `git push origin :refs/tags/vX.Y.Z`), fix the problem on `main`, then tag
  again.
- *It was published:* never move the tag. Ship the fix as X.Y.Z+1.

## Release notes

- **First line:** one sentence on what this release is about.
- **One section per area** (`## Search`, `## Results`, `## Round trips`, `## Installer and
  updates`, ...). Each bullet starts with a bold lead, then says in plain words what changed for the
  user and why it matters.
- **When a release removes something, or changes files or settings,** add "What to do, if it
  applies to you" and "Going back".
- **Say anything about updating itself,** e.g. that the update to this version is still run by the
  old version's updater.
- **End with**
  `**Full Changelog**: https://github.com/Giorgi-Apriashvili/WizzairHelper/compare/vPREV...vNEW`.
- **Leave out** internal jargon, commit hashes and attribution lines.

## Checking the installer locally

`cmake --build --preset dist` needs Inno Setup 6 (`winget install JRSoftware.InnoSetup`). A silent
install into a scratch folder, its version, and the silent uninstall:

```powershell
$setup = (Resolve-Path build\msvc-x64\dist\WizzairHelper-*-win64-setup.exe).Path
$dir = "$env:TEMP\WizzairHelper-install-test"
$p = Start-Process $setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=$dir" -PassThru
$p.WaitForExit(); $p.ExitCode                                         # 0
& "$dir\WizzairHelper.exe" --version | ForEach-Object { "$_" }        # WizzairHelper X.Y.Z
Start-Process "$dir\unins000.exe" -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait
```

Add `/RELAUNCH` to the Setup arguments to see the app start after the install. Wait for Setup with
`WaitForExit()`, not `Start-Process -Wait`: PowerShell's `-Wait` also waits for the app that Setup
started.

**The icon where users see it** (DECISIONS D-51) is checked by eye, because the `AppIcon` tests
cover the exe's icon group and the window icon but not the shell. For a release that changes the
icon or the installer, run Setup without `/VERYSILENT` and check that Setup itself, the Start-menu
entry, the optional desktop shortcut, *Settings > Apps* and the uninstaller show the pin icon, not
the generic program icon, and that the title bar and taskbar button do at 100 %, 150 % and 200 %
display scaling. Windows caches icons: if an old one shows, sign out and in before judging.

**The look where only Windows can switch it** (DECISIONS D-53..D-57). The tests switch light,
dark and High Contrast through the app's own switches, in-process and end to end, but nobody
toggles Windows itself in CI. For a release that changes the look, start the installed app and,
while it shows results:

- switch *Settings > Personalization > Colors > Choose your app mode* between Light and Dark: the
  window, its title bar, the grids, the open calendar and a tooltip follow at once, without a
  restart;
- turn on a contrast theme (*Settings > Accessibility > Contrast themes*, e.g. Night sky): the app
  shows the theme's colours with the plain Windows controls and stays readable; turn it off again
  and the look comes back;
- if a Windows 10 machine or real 150 % / 200 % monitors are at hand, check the window at the
  default and the minimum size there (both are simulated otherwise, RELEASE_CHECKS.md).

## Checklist

```
[ ] owner said "release"; patch or minor decided
[ ] CI green on HEAD (msvc-release, msvc-debug and clang-cl-release)
[ ] icon or installer changed: installed by hand, icons checked (Checking the installer locally)
[ ] look changed: live light/dark switch and a contrast theme checked by hand (Checking the installer locally)
[ ] notes written in release-notes-X.Y.Z.md
[ ] version bumped in CMakeLists.txt only; commit "Release X.Y.Z"
[ ] git tag vX.Y.Z; git push origin main; git push origin vX.Y.Z
[ ] tools/tag_wait.ps1 -Tag vX.Y.Z: both build legs + release job succeed
[ ] gh release edit vX.Y.Z --notes-file release-notes-X.Y.Z.md
[ ] tools/release_check.ps1 -Tag vX.Y.Z -Exe WizzairHelper.exe: assets, SHA256SUMS ok,
    WizzairHelper.exe reports X.Y.Z, releases/latest = vX.Y.Z
[ ] tools/rel-vX.Y.Z deleted; release recorded in docs/RELEASE_CHECKS.md
```
