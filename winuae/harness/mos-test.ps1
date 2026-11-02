# GTA under MorphOS 3.20 (QEMU pegasos2) in GTA's own lab, E:\morphos_gta
# (mos-lab-setup.ps1 makes it). Unattended:
#   1. the game (C:\temp\amiga_gta\work-aros, without logs and frames) goes on
#      a CD image, volume GTA:, with startlevel 0 and an autodrive script that
#      films, dumps and ends the game itself (quittest);
#   2. xfer\mostest copies it to RAM:, runs $Bin, and brings gta.log and the
#      frames back to ide0: (the guest must not write its log to ide0:);
#   3. QEMU boots, a shell is opened through the QEMU monitor (never host
#      input), `execute ide0:mostest` is typed into the GUEST, and a
#      screendump is taken every 20 s until xfer\stage.txt says END.
param([string]$Bin = 'AmiGTA', [int]$MaxMin = 20, [string]$Tag = 'g')
$d = 'E:\morphos_gta'; $s = ($d -replace '\\', '/') + '/shots'
$stage = 'C:\temp\amiga_gta\mos_iso'
function Mon($c) { powershell -ExecutionPolicy Bypass -File "$d\mon.ps1" $c | Out-Null }
function Multi($c, $p = 70) { powershell -ExecutionPolicy Bypass -File "$d\mon-multi.ps1" -Cmds $c -Pause $p }
function TypeG($t) { powershell -ExecutionPolicy Bypass -File "$d\type.ps1" -Text $t }

# our QEMU only - by the pid file, never by name
if (Test-Path "$d\qemu.pid") {
    $old = Get-Process -Id (Get-Content "$d\qemu.pid") -ErrorAction SilentlyContinue
    if ($old -and $old.ProcessName -eq 'qemu-gta-ppc') { Mon 'quit'; Start-Sleep 3 }
}

# 1. the CD
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\Gta" | Out-Null
robocopy 'C:\temp\amiga_gta\work-aros' "$stage\Gta" /E /NFL /NDL /NJH /NJS /NP `
    /XF *.raw gta.log reload.txt autodrive.txt startlevel.txt run | Out-Null
Copy-Item 'C:\temp\amiga_gta\work\AmiGTA' "$stage\Gta\" -Force
if (Test-Path 'C:\temp\amiga_gta\mos-build\AmiGTA_MorphOS') {
    Copy-Item 'C:\temp\amiga_gta\mos-build\AmiGTA_MorphOS' "$stage\Gta\" -Force
}
Set-Content "$stage\Gta\startlevel.txt" '0' -Encoding ascii
@('wait 400', 'film 1', 'wait 300', 'film 1', 'dump', 'wait 1500', 'quittest') |
    Set-Content "$stage\Gta\autodrive.txt" -Encoding ascii
wsl sh -c "genisoimage -quiet -R -J -V GTA -o /mnt/e/morphos_gta/gta-game.iso /mnt/c/temp/amiga_gta/mos_iso"

# 2. the guest script (LF line ends; AmigaDOS)
$lines = @(
  'FailAt 30', 'Stack 2000000', 'MakeDir RAM:Gta', 'Copy GTA:Gta/#? RAM:Gta ALL QUIET',
  'Copy GTA:Gta/startlevel.txt RAM:Gta/', 'Copy GTA:Gta/autodrive.txt RAM:Gta/',
  'CD RAM:Gta', 'Echo start >>ide0:stage.txt', ($Bin + ' >gta.log'),
  'Copy gta.log ide0:gta-mos.log', 'Copy live00.raw ide0:', 'Copy live01.raw ide0:',
  'Copy frame_live.raw ide0:', 'Echo END >>ide0:stage.txt')
Remove-Item "$d\xfer\*" -Force -ErrorAction SilentlyContinue
[IO.File]::WriteAllText("$d\xfer\mostest", (($lines -join "`n") + "`n"))

# 3. boot, shell, run
powershell -ExecutionPolicy Bypass -File "$d\start-mos.ps1"
$corner = ((1..10) | ForEach-Object { 'mouse_move -50 -50' }) -join ';'
Start-Sleep 100
Multi "mouse_move 0 92;mouse_move 0 93;mouse_button 1;mouse_button 0"          # language window OK
Start-Sleep 45
Multi "$corner;mouse_move 150 150;mouse_button 1;mouse_button 0"               # empty desktop
Multi "$corner;mouse_move 10 5;mouse_button 1;mouse_button 0"                  # Ambient menu
Start-Sleep 2
Multi "mouse_move 6 11;mouse_move 6 11;mouse_move 4 11;mouse_button 1;mouse_button 0" 80   # New Shell
Start-Sleep 25
TypeG "execute ide0:mostest"
$t0 = Get-Date; $i = 0
while (((Get-Date) - $t0).TotalMinutes -lt $MaxMin) {
    Start-Sleep 4; $i++
    Mon "screendump $s/$Tag-$i.png -f png"
    if ((Test-Path "$d\xfer\stage.txt") -and (Select-String -Path "$d\xfer\stage.txt" -Pattern 'END' -Quiet)) { "END after $i shots"; break }
}
Mon "screendump $s/$Tag-end.png -f png"
Mon 'quit'
