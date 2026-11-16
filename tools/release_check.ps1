# Checks a published GitHub release: draft/prerelease flags, its assets, every SHA256SUMS.txt line
# against the downloaded file, the zip's program reporting its version (with an isolated profile, so a
# first run can't touch your real settings), and that releases/latest points at it.
# Downloads into .\rel-<tag> next to this script; delete it afterwards.
# Example: pwsh release_check.ps1 -Repo owner/name -Tag v1.2.3 -Exe app.exe
param(
    [string]$Repo = 'Giorgi-Apriashvili/WizzairHelper',
    [Parameter(Mandatory)][string]$Tag,
    [string]$Exe = '',               # a program inside the zip to run, e.g. app.exe; empty: skip
    [string]$VersionArg = '--version'
)
$ErrorActionPreference = 'Stop'

$cred = "protocol=https`nhost=github.com`n`n" | git credential fill 2>$null
$token = ($cred | Where-Object { $_ -like 'password=*' }) -replace '^password=', ''
$h = @{ 'User-Agent' = 'release-check'; 'Accept' = 'application/vnd.github+json' }
if ($token) { $h['Authorization'] = "Bearer $token" }
$api = "https://api.github.com/repos/$Repo/releases"

$rel = Invoke-RestMethod -Headers $h "$api/tags/$Tag"
"release $($rel.tag_name): draft=$($rel.draft) prerelease=$($rel.prerelease) published $($rel.published_at)"
$rel.assets | ForEach-Object { '  {0,-45} {1,12:N0} bytes' -f $_.name, $_.size }

$dir = Join-Path $PSScriptRoot "rel-$Tag"
New-Item -ItemType Directory -Force $dir | Out-Null
foreach ($a in $rel.assets) {
    Invoke-WebRequest -Headers @{ 'User-Agent' = 'release-check' } -Uri $a.browser_download_url -OutFile (Join-Path $dir $a.name)
}

$sumsFile = Join-Path $dir 'SHA256SUMS.txt'
if (Test-Path $sumsFile) {
    $sums = Get-Content $sumsFile | Where-Object { $_.Trim() }
    $bad = 0
    foreach ($line in $sums) {
        $parts = $line -split '\s+\*?', 2
        $want = $parts[0].ToLower(); $name = $parts[1].Trim().TrimStart('*')
        $path = Join-Path $dir $name
        if (-not (Test-Path $path)) { "  MISSING $name"; $bad++; continue }
        $got = (Get-FileHash $path -Algorithm SHA256).Hash.ToLower()
        if ($got -eq $want) { "  sha256 ok  $name" } else { "  sha256 BAD $name"; $bad++ }
    }
    "SHA256SUMS: $($sums.Count) lines, $bad problem(s)"
} else {
    "SHA256SUMS.txt MISSING"
}

$zip = Get-ChildItem $dir -Filter '*.zip' | Select-Object -First 1
if ($zip) {
    $x = Join-Path $dir 'zip'
    Expand-Archive $zip.FullName $x -Force
    "zip contents: $((Get-ChildItem $x -Recurse -File | ForEach-Object { $_.Name }) -join ', ')"
    if ($Exe) {
        $prog = Get-ChildItem $x -Recurse -Filter $Exe | Select-Object -First 1
        if (-not $prog) { "$Exe not found in the zip" }
        else {
            $iso = Join-Path $dir 'iso'; New-Item -ItemType Directory -Force $iso, "$iso\local" | Out-Null
            $saved = $env:APPDATA, $env:LOCALAPPDATA
            $env:APPDATA = $iso; $env:LOCALAPPDATA = "$iso\local"
            # Piped: PowerShell captures and waits for a GUI-subsystem program only inside a pipeline.
            try { "$Exe $VersionArg`: $(& $prog.FullName $VersionArg 2>&1 | ForEach-Object { "$_" })" }
            finally { $env:APPDATA, $env:LOCALAPPDATA = $saved }
        }
    }
}

$latest = Invoke-RestMethod -Headers $h "$api/latest"
"releases/latest: $($latest.tag_name)"
"downloaded to $dir (delete it afterwards)"
