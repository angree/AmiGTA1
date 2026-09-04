# Restart the game INSIDE the running emulator and wait for it to settle.
#
# Drops Work:reload.txt; the game deletes it and exits with RC 5, and the run
# script starts AmiGTA again from the drawer - so whatever tools/bin/deploy.sh
# put there (binary, autowalk.txt, autodrive.txt, opts.txt) is what runs.
# No WinUAE is started or killed: that took the keyboard and the mouse from
# the developer working at the same machine, every time, and was the single
# most objected-to thing this harness did.
#
# Usage:
#   reload-gta.ps1                         # wait for "gta: interactive"
#   reload-gta.ps1 -WaitFor "autodrive done" -TimeoutSec 180
#
# Refuses if no gta-*.uae emulator is running: start one with run-gta.ps1
# (once), and not again.
param(
  [int]$TimeoutSec = 120,
  [string]$WaitFor = "gta: interactive"
)
$work = "C:\temp\amiga_gta\work"
$log = Join-Path $work "gta.log"
$flag = Join-Path $work "reload.txt"

$running = Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'winuae*' -and $_.CommandLine -match 'gta-[a-z0-9-]*\.uae' }
if (-not $running) { Write-Output "ERROR: no gta-*.uae emulator is running - start one with run-gta.ps1"; exit 1 }

# The new instance truncates gta.log with ">" when it starts, so "the log got
# smaller" is the sign that the old game has left and the new one is up.
$before = if (Test-Path $log) { (Get-Item $log).Length } else { -1 }
Set-Content -Path $flag -Value "reload" -Encoding ascii
Write-Output "reload requested (log was $before bytes)"

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$restarted = $false
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 500
  if (-not $restarted) {
    if ((Test-Path $log) -and ((Get-Item $log).Length -lt $before -or $before -lt 0)) { $restarted = $true }
    elseif (-not (Test-Path $flag) -and $before -eq 0) { $restarted = $true }
    continue
  }
  if ((Test-Path $log) -and (Select-String -Path $log -SimpleMatch -Pattern $WaitFor -Quiet)) {
    Write-Output "--- gta.log ---"
    Get-Content $log
    exit 0
  }
}
Write-Output "TIMEOUT: no '$WaitFor' after $TimeoutSec s (restarted: $restarted, flag still there: $(Test-Path $flag))"
if (Test-Path $log) { Write-Output "--- gta.log (last 30 lines) ---"; Get-Content $log -Tail 30 }
exit 1
