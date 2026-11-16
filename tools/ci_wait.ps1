# Waits for the GitHub Actions run of a commit, then prints each job's result, minutes and failed step.
# The API token comes from git's credential store, in memory only (never printed).
# Example: pwsh ci_wait.ps1 -Repo owner/name -Sha (git rev-parse HEAD)
param(
    [string]$Repo = 'Giorgi-Apriashvili/WizzairHelper',
    [Parameter(Mandatory)][string]$Sha,
    [int]$MaxMinutes = 45,
    [int]$EverySeconds = 60
)

$cred = "protocol=https`nhost=github.com`n`n" | git credential fill 2>$null
$token = ($cred | Where-Object { $_ -like 'password=*' }) -replace '^password=', ''
$h = @{ 'User-Agent' = 'ci-wait'; 'Accept' = 'application/vnd.github+json' }
if ($token) { $h['Authorization'] = "Bearer $token" }
$api = "https://api.github.com/repos/$Repo/actions"
$deadline = (Get-Date).AddMinutes($MaxMinutes)
while ((Get-Date) -lt $deadline) {
    $runs = (Invoke-RestMethod -Headers $h "$api/runs?head_sha=$Sha&per_page=5").workflow_runs
    $run = $runs | Select-Object -First 1
    if ($run -and $run.status -eq 'completed') {
        "run $($run.id): $($run.conclusion)  $($run.html_url)"
        (Invoke-RestMethod -Headers $h "$api/runs/$($run.id)/jobs").jobs | ForEach-Object {
            $mins = if ($_.completed_at) { ([datetime]$_.completed_at - [datetime]$_.started_at).TotalMinutes } else { 0 }
            '  {0,-20} {1,-10} {2:0.0} min' -f $_.name, $_.conclusion, $mins
            if ($_.conclusion -ne 'success' -and $_.conclusion -ne 'skipped') {
                $_.steps | Where-Object { $_.conclusion -eq 'failure' } | ForEach-Object { "    failed step: $($_.name)" }
            }
        }
        return
    }
    Start-Sleep -Seconds $EverySeconds
}
"timed out after $MaxMinutes min"
