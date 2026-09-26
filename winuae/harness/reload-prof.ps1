# Restart the game INSIDE the running PROFILING guest (gta-prof*.uae) and
# wait for it to settle - the reload-gta.ps1 of the isolated runtime.
#
# The profiling machine used to be started and killed by run-prof.ps1 for
# every measurement. Each start takes the keyboard and the mouse from the
# developer working at the same machine (CLAUDE.md), so it is started ONCE
# now, with `run-prof.ps1 -Config gta-prof80.uae -KeepRunning`, and every
# arm of every A/B pair after that goes through here: drop Work:reload.txt
# in the prof runtime, the game exits with RC 5, the run script starts
# whatever binary and opts.txt are in the drawer now.
#
#   reload-prof.ps1                                  # wait for "gta: interactive"
#   reload-prof.ps1 -WaitFor "gta: interactive" -Collect 150   # ...then watch
#   reload-prof.ps1 -Work C:\temp\amiga_gta_prof2\work
#
# Prints the log at the end so the caller can grep it; copies nothing.
param(
  [string]$Work = "C:\temp\amiga_gta_prof\work",
  [string]$WaitFor = "gta: interactive",
  [int]$TimeoutSec = 420,
  [int]$Collect = 0
)
$log = Join-Path $Work "gta.log"
$flag = Join-Path $Work "reload.txt"

$running = Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'winuae*' -and $_.CommandLine -match 'gta-prof[a-z0-9-]*\.uae' }
if (-not $running) { Write-Output "ERROR: no gta-prof*.uae guest is running - start one with run-prof.ps1 -KeepRunning"; exit 1 }

$before = if (Test-Path $log) { (Get-Item $log).Length } else { -1 }
Set-Content -Path $flag -Value "reload" -Encoding ascii
Write-Output "reload requested in $Work (log was $before bytes)"

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$restarted = $false
$seen = $false
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 500
  if (-not $restarted) {
    if ((Test-Path $log) -and ((Get-Item $log).Length -lt $before -or $before -lt 0)) { $restarted = $true }
    elseif (-not (Test-Path $flag) -and $before -eq 0) { $restarted = $true }
    continue
  }
  if ((Test-Path $log) -and (Select-String -Path $log -SimpleMatch -Pattern $WaitFor -Quiet)) { $seen = $true; break }
}
if (-not $seen) {
  Write-Output "TIMEOUT: no '$WaitFor' after $TimeoutSec s (restarted: $restarted, flag still there: $(Test-Path $flag))"
  if (Test-Path $log) { Get-Content $log -Tail 20 }
  exit 1
}
if ($Collect -gt 0) { Write-Output "collecting for $Collect s..."; Start-Sleep $Collect }
Write-Output "--- gta.log ---"
Get-Content $log
exit 0
