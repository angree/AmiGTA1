# Start the ISOLATED profiling machine - gta-prof.uae, whose Work: is
# C:\temp\amiga_gta_prof\work and NOT the shared C:\temp\amiga_gta\work.
#
# WHY A SECOND RUNTIME EXISTS. Every gta-*.uae config mounts the same Work:,
# so run-gta.ps1 refuses to start while another guest is alive - two guests
# would write one gta.log and both runs would be garbage. That rule is right,
# and it also means a measurement cannot be taken while the developer is
# playing in a guest somebody left running. This config has its own copy of
# the runtime (ROM, hardfile, Work:), so it can run BESIDE theirs and neither
# one can touch the other's evidence.
#
# It still kills only its own instances, matched on gta-prof.uae, and it never
# touches winuae processes belonging to any other project.
#
#   run-prof.ps1                       # start, wait for the profile, kill
#   run-prof.ps1 -Collect 120          # ...and watch it for two more minutes
#   run-prof.ps1 -KeepRunning          # leave it up
param(
  [string]$Config = "gta-prof.uae",
  [string]$WaitFor = "gta: profile",
  [int]$TimeoutSec = 420,
  [int]$Collect = 0,
  [switch]$KeepRunning
)

$exe = "I:\GITHUB\Amiga_OpenTTD\tools\winuae281\winuae-gta.exe"
$wd  = "I:\GITHUB\Amiga_OpenTTD\tools\winuae281"
$cfg = Join-Path "I:\GITHUB\Amiga_GTA\winuae" $Config
# Each prof config mounts its own runtime; the log lives beside that Work.
# Derived from the config name: gta-prof80b.uae -> amiga_gta_prof2.
$log = if ($Config -match "80b") { "C:\temp\amiga_gta_prof2\work\gta.log" }
       else { "C:\temp\amiga_gta_prof\work\gta.log" }

function Stop-Prof {
  $ours = @(Get-CimInstance Win32_Process -Filter "Name LIKE 'winuae%'" |
            Where-Object { $_.CommandLine -match $Config.Replace(".","\.") })
  foreach ($p in $ours) {
    Write-Output ("killing our profiling guest, pid {0}" -f $p.ProcessId)
    Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
  }
  # Say what was deliberately left alone - other projects share the binary.
  $others = @(Get-CimInstance Win32_Process -Filter "Name LIKE 'winuae%'" |
              Where-Object { $_.CommandLine -notmatch $Config.Replace(".","\.") })
  foreach ($p in $others) {
    Write-Output ("left alone: pid {0}  {1}" -f $p.ProcessId, $p.CommandLine)
  }
}

if (-not (Test-Path $exe)) { Write-Output "ERROR: WinUAE not found at $exe"; exit 1 }
if (-not (Test-Path $cfg)) { Write-Output "ERROR: config not found: $cfg"; exit 1 }

Stop-Prof
Start-Sleep 2

# EVERY gta-prof* config mounts the SAME Work:, so a second profiling guest
# would write into this one's gta.log. That happened once and the numbers it
# produced looked plausible and were somebody else's machine entirely - a
# throttled 68020 reporting 60 fps. Refuse rather than measure a mixture.
#
# It happens more easily than it sounds: piping this script through
# `Select-Object -First n` terminates it early - PowerShell stops the upstream
# pipeline - so the Stop-Prof at the bottom never runs and the guest is left
# alive. Filter the output with Select-String, never with -First.
# Only THIS config's guests share this config's Work: - prof and prof2 are
# separate runtimes now, so a guest on the other one is a neighbour, not a
# conflict.
$stray = @(Get-CimInstance Win32_Process -Filter "Name LIKE 'winuae%'" |
           Where-Object { $_.CommandLine -match $Config.Replace(".","\.") })
if ($stray.Count -gt 0) {
  Write-Output "ANOTHER PROFILING GUEST IS ALIVE - it shares Work: with this one:"
  $stray | ForEach-Object { Write-Output ("  pid {0}  {1}" -f $_.ProcessId, $_.CommandLine) }
  Write-Output "refusing to start; kill it first."
  exit 3
}

Remove-Item $log -ErrorAction SilentlyContinue
Start-Process -FilePath $exe -ArgumentList '-log', '-f', $cfg -WorkingDirectory $wd

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$seen = $false
while ((Get-Date) -lt $deadline) {
  if ((Test-Path $log) -and (Select-String -Path $log -SimpleMatch -Pattern $WaitFor -Quiet)) {
    $seen = $true
    break
  }
  Start-Sleep 3
}

if (-not $seen) {
  Write-Output "TIMEOUT: no '$WaitFor' in $log after $TimeoutSec s"
} elseif ($Collect -gt 0) {
  Write-Output "collecting for $Collect s..."
  Start-Sleep $Collect
}

Write-Output "--- gta.log ---"
if (Test-Path $log) { Get-Content $log }
if (-not $KeepRunning) { Stop-Prof }
if ($seen) { exit 0 } else { exit 1 }
