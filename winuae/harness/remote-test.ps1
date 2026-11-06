# One test of the current build on the second PC, through the gate (I:\temp\gate),
# in its own emulator, so a window the developer is using there is not touched.
#
#   remote-test.ps1 -Script tools\scripts\foo.txt [-Level 0] [-Opts 'mips 1'] [-Tag x]
#                   [-WaitFor 'autodrive done'] [-TimeoutSec 400]
#   remote-test.ps1 -Stop      close the test emulator
#   -Config gta-2pc-aros.uae   the same machine on the AROS ROM; switching
#                              config restarts the test emulator
#
# The emulator is started once and stays up; later tests reload the game in it.
# It is shown without being activated, so it takes neither focus nor mouse.
# Back here, in C:\temp\amiga_gta\remote\<Tag>\: gta.log, every *.raw the game
# dumped, and PNGs of them. The window itself is never captured: the game's own
# frame dumps are the screenshots.
param(
    [string]$Script = '',
    [int]$Level = 0,
    [string]$Opts = 'mips 1',
    [string]$Tag = 'r',
    [string]$WaitFor = 'autodrive done',
    [int]$TimeoutSec = 400,
    [string]$Config = 'gta-2pc-test.uae',
    [switch]$Stop
)
$gate = 'I:\temp\gate\gate.ps1'
$stage = 'I:\temp\amiga_gta_2pc\test'
$jobFile = Join-Path $env:TEMP "remote-test-$PID.ps1"
$remoteJob = @'
$s = '__STAGE__'; $c = 'C:\temp\amiga_gta'; $w = "$c\work-test"; $exe = "$c\uae\winuae-gtatest.exe"
$cfg = "$c\__CONFIG__"
$running = Get-CimInstance Win32_Process -Filter "Name='winuae-gtatest.exe'"
# another config in the running one: it has to go, the ROM is not swapped live
$other = @($running | Where-Object { $_.CommandLine -notlike "*__CONFIG__*" })
if ($other.Count -gt 0 -and '__STOP__' -ne '1') {
    $other | ForEach-Object { Stop-Process -Id $_.ProcessId -Force; "stopped $($_.ProcessId) (other config)" }
    Start-Sleep 2
    $running = Get-CimInstance Win32_Process -Filter "Name='winuae-gtatest.exe'"
}
if ('__STOP__' -eq '1') { $running | ForEach-Object { Stop-Process -Id $_.ProcessId -Force; "stopped $($_.ProcessId)" }; exit 0 }
if (-not (Test-Path $w)) {
    & robocopy.exe "$c\work" $w /E /NP /NFL /NDL /NJH /NJS /R:1 /W:1 /XF gta.log *.raw screen.bin run hook testhook up.txt press.log do boot-wb | Out-Null
}
if (-not (Test-Path $exe)) { Copy-Item "$c\uae\winuae-gta.exe" $exe }
Copy-Item "$s\__CONFIG__" $cfg -Force
Copy-Item "$s\AmiGTA", "$s\run", "$s\autodrive.txt", "$s\opts.txt", "$s\startlevel.txt" $w -Force
Remove-Item "$w\*.raw" -ErrorAction SilentlyContinue
$log = "$w\gta.log"
if ($running) {
    # The new game truncates the log when it starts: "the log got smaller" is
    # the sign that the old one has left. Its text must not satisfy the wait.
    $before = if (Test-Path $log) { (Get-Item $log).Length } else { -1 }
    [IO.File]::WriteAllText("$w\reload.txt", "1`n")
    $t0 = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $t0 -and (Test-Path $log) -and (Get-Item $log).Length -ge $before) { Start-Sleep -Milliseconds 300 }
    $restarted = (Get-Item $log -ErrorAction SilentlyContinue).Length -lt $before
    "restarted: $restarted"
    # Not restarted means the log is the OLD run's: reporting from it passed a
    # stale result off as this test (a pause.txt log came back for drown.txt).
    if (-not $restarted) { 'FAILED: the old game did not leave within 60 s'; exit 2 }
    $proc = Get-Process -Id $running[0].ProcessId
    if (@($running).Count -gt 1) { 'WARNING: more than one test emulator' }
} else {
    Remove-Item "$w\reload.txt", $log -ErrorAction SilentlyContinue
    $proc = Start-Process -FilePath $exe -ArgumentList '-log', '-f', $cfg -WorkingDirectory "$c\uae" -WindowStyle Minimized -PassThru
    Add-Type -Name W -Namespace RemoteTest -MemberDefinition '
[DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int c);
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint wp);
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
'
    # A minimised WinUAE 2.8.1 is paused: show it, without activating it.
    $script:shown = $false
    for ($i = 0; $i -lt 40 -and -not $script:shown; $i++) {
        Start-Sleep -Milliseconds 500
        $cb = [RemoteTest.W+EnumProc]{
            param($h, $l)
            [uint32]$wp = 0
            [RemoteTest.W]::GetWindowThreadProcessId($h, [ref]$wp) | Out-Null
            if ($wp -eq $proc.Id) {
                $n = New-Object Text.StringBuilder 64
                [RemoteTest.W]::GetClassName($h, $n, 64) | Out-Null
                if ($n.ToString() -eq 'PCsuxRox') { [RemoteTest.W]::ShowWindowAsync($h, 4) | Out-Null; $script:shown = $true }
            }
            return $true
        }
        [RemoteTest.W]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    }
    "started pid $($proc.Id) shown=$($script:shown)"
}
$until = (Get-Date).AddSeconds(__TIMEOUT__)
$seen = $false
while ((Get-Date) -lt $until -and -not $proc.HasExited) {
    if ((Test-Path $log) -and (Select-String -Path $log -SimpleMatch -Pattern '__WAITFOR__' -Quiet)) { $seen = $true; break }
    Start-Sleep 2
}
"seen '__WAITFOR__': $seen, emulator alive: $(-not $proc.HasExited)"
Start-Sleep 1
Remove-Item "$s\out" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$s\out" | Out-Null
if (Test-Path $log) { Copy-Item $log "$s\out\gta.log" -Force }
Get-ChildItem $w -Filter *.raw -ErrorAction SilentlyContinue | Copy-Item -Destination "$s\out" -Force
if (-not $seen) { exit 1 }
'@
$body = $remoteJob.Replace('__STAGE__', $stage).Replace('__TIMEOUT__', "$TimeoutSec").Replace('__WAITFOR__', $WaitFor).Replace('__STOP__', $(if ($Stop) { '1' } else { '0' })).Replace('__CONFIG__', $Config)
Set-Content -LiteralPath $jobFile -Value $body -Encoding UTF8
if (-not $Stop) {
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item 'C:\temp\amiga_gta_build\build\AmiGTA' "$stage\AmiGTA" -Force
    Copy-Item "I:\GITHUB\Amiga_GTA\winuae\$Config" "$stage\$Config" -Force
    if ($Script) {
        # relative to the repo, not to wherever the shell happens to stand
        if (-not [IO.Path]::IsPathRooted($Script)) { $Script = Join-Path 'I:\GITHUB\Amiga_GTA' $Script }
        Copy-Item $Script "$stage\autodrive.txt" -Force -ErrorAction Stop
    } else { Set-Content "$stage\autodrive.txt" 'wait 500' -Encoding ascii }
    $tpl = (Get-Content -Raw 'I:\GITHUB\Amiga_GTA\winuae\work-template\run') -replace "`r`n", "`n"
    $tpl = $tpl -replace '(?m)^Stack 1000000$', "Stack 1000000`nCD Work:"
    [IO.File]::WriteAllText("$stage\run", $tpl)
    Set-Content "$stage\opts.txt" $Opts -Encoding ascii
    Set-Content "$stage\startlevel.txt" "$Level" -Encoding ascii
}
powershell -NoProfile -ExecutionPolicy Bypass -File $gate -Timeout ($TimeoutSec + 60) -File $jobFile
$rc = $LASTEXITCODE
Remove-Item -LiteralPath $jobFile -ErrorAction SilentlyContinue
if ($Stop) { exit $rc }
$out = "C:\temp\amiga_gta\remote\$Tag"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$stage\out\*" $out -Force -ErrorAction SilentlyContinue
Get-ChildItem $out -Filter *.raw -ErrorAction SilentlyContinue | ForEach-Object {
    python I:\GITHUB\Amiga_GTA\tools\bin\raw2png.py $_.FullName ($_.FullName -replace '\.raw$', '.png') 2>&1 | Out-Null
}
"results in $out"
Get-ChildItem $out | ForEach-Object { '  {0} {1}' -f $_.Name, $_.Length }
exit $rc
