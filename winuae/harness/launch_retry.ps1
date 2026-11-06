# Launch WinUAE and verify the emulated system actually boots (detected via
# the logpump heartbeat file Work:ramlist.txt). Retries on dead boots
# (green/black HALT at kickstart, seen after rapid kill/relaunch cycles).
# Usage: launch_retry.ps1 [-Config path] [-Tries 3] [-BootTimeoutSec 75]
param(
  [string]$Config = "I:\GITHUB\Amiga_OpenTTD\winuae\otd-run-281.uae",
  [int]$Tries = 3,
  [int]$BootTimeoutSec = 75
)
$exe = "I:\GITHUB\Amiga_OpenTTD\tools\winuae281\winuae.exe"
$wd  = "I:\GITHUB\Amiga_OpenTTD\tools\winuae281"
$hb  = "I:\GITHUB\Amiga_OpenTTD\winuae\work\ramlist.txt"

for ($t = 1; $t -le $Tries; $t++) {
  & (Join-Path $PSScriptRoot "kill_ours.ps1") | Out-Null   # ONLY ours - never Stop-Process -Name winuae
  Start-Sleep 6
  Remove-Item $hb -ErrorAction SilentlyContinue
# THE DEVELOPER'S RULE (2026-11-05): never weaker than -70% CPU, JIT or not.
$thr = [regex]::Match((Get-Content -LiteralPath $Config -Raw), '(?m)^cpu_throttle=(-?[\d.]+)')
if (-not $thr.Success -or [double]::Parse($thr.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) -gt -700) {
  Write-Output "REFUSED: $Config must have cpu_throttle=-700 or lower"
  exit 4
}
  Start-Process -FilePath $exe -ArgumentList '-f', $Config -WorkingDirectory $wd
  $deadline = (Get-Date).AddSeconds($BootTimeoutSec)
  while ((Get-Date) -lt $deadline) {
    if (Test-Path $hb) { Write-Output "BOOTED (try $t)"; exit 0 }
    Start-Sleep 3
  }
  Write-Output "dead boot on try $t, retrying"
}
Write-Output "FAILED: no boot after $Tries tries"
exit 1
