# Start WinUAE on one of the GTA configs and wait for the game's log to
# appear, so an agent-driven test run needs no human at the emulator.
#
# WinUAE MUST be started as "winuae.exe -f <config>" with use_gui=no in the
# config: anything else opens the configuration window and sits there waiting
# for a person. That is the single most common way to waste a test cycle.
#
# SEVERAL AGENTS SHARE THIS MACHINE, and that changes what this script may do.
# There can be more than one Claude session working on this port at the same
# time, plus the author's own runs, so two rules apply and both are enforced
# below rather than remembered:
#
#   1. Kill only instances of the CONFIG WE ARE ABOUT TO START, not every
#      gta-*.uae. The old code killed all of ours, which is right for one agent
#      and wrong for three - somebody else's gta-rtg measurement is not ours to
#      end just because we want gta-aga.
#   2. REFUSE TO START if another gta-*.uae is running at all. Every config
#      mounts the same C:\temp\amiga_gta\work as Work:, so two guests share one
#      gta.log and one frame.raw and both runs produce garbage. That is worse
#      than not running: it silently corrupts someone else's evidence. Pass
#      -Force to override, and only when you know the other run is dead weight.
#
# Usage:
#   run-gta.ps1                      # AGA config, 90 s for the run to finish
#   run-gta.ps1 -Config gta-rtg.uae -TimeoutSec 180
#   run-gta.ps1 -Force               # start anyway with another guest running
#   run-gta.ps1 -WaitFor "benchmark" # settle for an earlier line than the default
#
# WHAT IT WAITS FOR, AND WHY IT IS NOT "the log exists".
# It used to return as soon as gta.log appeared, wait two seconds, and kill the
# guest. The game creates that file within a second of starting, so on the
# throttled measurement machine the emulator was being shot in the middle of
# the benchmark and the log simply stopped mid-sentence - which reads exactly
# like the game hanging. It now waits for a line that only gets printed when
# the unattended part is OVER.
param(
  [string]$Config = "I:\GITHUB\Amiga_GTA\winuae\gta-aga.uae",
  [int]$TimeoutSec = 90,
  [string]$WaitFor = "gta: interactive",
  [switch]$KeepRunning,
  [switch]$Force
)

$exe = "I:\GITHUB\Amiga_OpenTTD\tools\winuae281\winuae-gta.exe"
$wd  = "I:\GITHUB\Amiga_OpenTTD\tools\winuae281"
# The runtime (Work:, the hardfile, the Kickstart) lives on the SSD, not in the
# repository on the network drive - see the note in gta-aga.uae.
$log = "C:\temp\amiga_gta\work\gta.log"

if (-not (Test-Path $exe)) { Write-Output "ERROR: WinUAE not found at $exe"; exit 1 }
if (-not (Test-Path $Config)) { Write-Output "ERROR: config not found: $Config"; exit 1 }

# Only this config's own instances - see rule 1 at the top.
$cfgName = [System.IO.Path]::GetFileName($Config)
& (Join-Path $PSScriptRoot "kill_ours.ps1") -Config $cfgName | Out-Null
Start-Sleep 3

# Rule 2: anything else of ours still alive shares Work: with us.
$others = @(Get-CimInstance Win32_Process -Filter "Name LIKE 'winuae%.exe'" |
            Where-Object { $_.CommandLine -match "gta-[a-z0-9-]*\.uae" })
if ($others.Count -gt 0) {
  Write-Output "ANOTHER GTA GUEST IS RUNNING - it shares Work: with this one:"
  $others | ForEach-Object { Write-Output ("  pid {0}  {1}" -f $_.ProcessId, $_.CommandLine) }
  if (-not $Force) {
    Write-Output "refusing to start. Another agent may be measuring in it."
    Write-Output "If it is dead weight: kill_ours.ps1 -Config <its-config>, or pass -Force."
    exit 3
  }
  Write-Output "-Force given: starting anyway."
}

Remove-Item $log -ErrorAction SilentlyContinue

# -log turns on winuaelog.txt in the WinUAE directory. Without it the only
# trace of a failed boot is the boot log, which stops before emulation starts -
# so a HALT or a Guru leaves nothing at all to read.
$uaelog = Join-Path $wd "winuaelog.txt"
Remove-Item $uaelog -ErrorAction SilentlyContinue
# Plain launch (raw input on). Synthetic mouse input reaches the emulator only
# as RELATIVE deltas while it has the mouse trapped - that is what
# click_ours.ps1 does, and it verifies every click against the game's log.
# (-nodirectinput -norawinput was tried for absolute SetCursorPos driving:
# WinUAE 2.8.1 then ignored synthetic moves entirely.)
# WinUAE 2.8.1 ignores win32.posx/posy in the config and opens the emulation
# window where winuae.ini remembers it (MainPosX/MainPosY). With gfx_api=0
# (DirectDraw) a window created on a SECONDARY monitor comes up black - for the
# user and for every capture - and stays black. That happened after the user
# dragged a running instance to their second monitor: every launch after it was
# a black window with a perfectly healthy game inside. So the remembered
# position is forced back onto the primary monitor before each start. Only new
# windows are affected; nothing running is touched.
$ini = Join-Path $wd "winuae.ini"
if (Test-Path $ini) {
  $txt = Get-Content $ini -Raw
  $txt = [regex]::Replace($txt, '(?m)^MainPosX=.*$', 'MainPosX=100')
  $txt = [regex]::Replace($txt, '(?m)^MainPosY=.*$', 'MainPosY=60')
  Set-Content $ini $txt -Encoding ASCII -NoNewline
}
Start-Process -FilePath $exe -ArgumentList '-log', '-f', $Config -WorkingDirectory $wd

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while ((Get-Date) -lt $deadline) {
  if ((Test-Path $log) -and (Select-String -Path $log -SimpleMatch -Pattern $WaitFor -Quiet)) {
    Start-Sleep 2
    Write-Output "--- gta.log ---"
    Get-Content $log
    if (-not $KeepRunning) {
      & (Join-Path $PSScriptRoot "kill_ours.ps1") -Config $cfgName | Out-Null  # this config only
    }
    exit 0
  }
  Start-Sleep 3
}

Write-Output "TIMEOUT: no '$WaitFor' in $log after $TimeoutSec s"
if (Test-Path $log) {
  Write-Output "--- gta.log (last 30 lines - how far it actually got) ---"
  Get-Content $log -Tail 30
}
if (Test-Path $uaelog) {
  Write-Output "--- winuaelog.txt (last 60 lines) ---"
  Get-Content $uaelog -Tail 60
}
if (-not $KeepRunning) {
  & (Join-Path $PSScriptRoot "kill_ours.ps1") -Config $cfgName | Out-Null  # this config only
}
exit 1
