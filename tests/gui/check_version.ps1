# CTest app.version.powershell and app.version.resource (D-35).
#
#   powershell -File check_version.ps1 -Exe <WizzairHelper.exe> -Version X.Y.Z            # --version, captured
#   powershell -File check_version.ps1 -Exe <WizzairHelper.exe> -Version X.Y.Z -Resource  # version resource
#
# The first form captures the output the way tools/release_check.ps1 does,
# $(& .\WizzairHelper.exe --version 2>&1 | ForEach-Object { "$_" }): one line
# "WizzairHelper X.Y.Z", exit code 0. PowerShell redirects and waits for a GUI-subsystem
# program only when it is piped into another command; as the last command of a pipeline,
# as in $(& .\WizzairHelper.exe --version 2>&1), it starts the program detached, and the
# line appears on the console (D-35) instead of in the captured value.
# The second form checks the exe's version resource against the same project version.
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Version,
    [switch]$Resource
)
$ErrorActionPreference = 'Stop'
$problems = @()

if ($Resource) {
    $info = (Get-Item -LiteralPath $Exe).VersionInfo
    $expected = [ordered]@{
        ProductName      = 'WizzairHelper'
        FileDescription  = 'WizzairHelper'
        OriginalFilename = 'WizzairHelper.exe'
        ProductVersion   = $Version
        FileVersion      = "$Version.0"
        FileVersionRaw   = "$Version.0"
        ProductVersionRaw = "$Version.0"
    }
    foreach ($name in $expected.Keys) {
        $actual = "$($info.$name)"
        if ($actual -ne $expected[$name]) { $problems += "$name is '$actual', expected '$($expected[$name])'" }
    }
} else {
    $output = $(& $Exe --version 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    $lines = @($output | ForEach-Object { "$_" })
    if ($code -ne 0) { $problems += "exit code $code, expected 0" }
    if ($lines.Count -ne 1 -or $lines[0] -cne "WizzairHelper $Version") {
        $problems += "captured [$($lines -join '|')], expected [WizzairHelper $Version]"
    }
}

if ($problems.Count -gt 0) {
    $problems | ForEach-Object { Write-Output "FAIL: $_" }
    exit 1
}
Write-Output 'OK'
