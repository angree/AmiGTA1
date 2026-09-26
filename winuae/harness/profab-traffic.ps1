# Anchored A/B of the traffic models on the profiling guest (gta-prof80.uae,
# plain 68020, JIT off, throttled). Same binary in every arm; only opts.txt's
# trafficlite differs. Arms interleaved: lite, full, lite, full, lite, full.
# gta.log is held open by the running game, so it cannot be deleted between
# arms: the run script truncates it when the game restarts (`AmiGTA >gta.log`),
# and that - "autodrive done" gone - is what marks the new arm.
param([int]$ArmTimeout = 1500)
$w = 'C:\temp\amiga_gta_prof\work'
$log = "$w\gta.log"
$out = 'C:\temp\amiga_gta_build\out'
Remove-Item "$out\prof_ab_*.log" -ErrorAction SilentlyContinue
Copy-Item 'C:\temp\amiga_gta\work\AmiGTA' "$w\AmiGTA" -Force
foreach ($f in 'autowalk.txt', 'autoinput.txt') {
    if (Test-Path "$w\$f") { Move-Item "$w\$f" "$w\$f.keep" -Force }
}
function Done { (Test-Path $log) -and (Select-String -Path $log -SimpleMatch -Pattern 'autodrive done' -Quiet) }
$arms = @('1', '0', '1', '0', '1', '0')
$first = $true
$n = 0
foreach ($arm in $arms) {
    Set-Content -Path "$w\opts.txt" -Value "audio 0`ntrafficlite $arm" -Encoding ascii
    Copy-Item 'I:\GITHUB\Amiga_GTA\tools\scripts\litetraffic_prof.txt' "$w\autodrive.txt" -Force
    $ok = $false
    if ($first) {
        powershell -ExecutionPolicy Bypass -File 'I:\GITHUB\Amiga_GTA\winuae\harness\run-prof.ps1' -Config gta-prof80.uae -WaitFor 'autodrive done' -TimeoutSec $ArmTimeout -KeepRunning | Select-String -Pattern 'TIMEOUT|ERROR|REFUS'
        $first = $false
        $ok = Done
    } else {
        Set-Content -Path "$w\reload.txt" -Value 'reload' -Encoding ascii
        $deadline = (Get-Date).AddSeconds($ArmTimeout)
        $restarted = $false
        while ((Get-Date) -lt $deadline) {
            if (-not $restarted) { if (-not (Done)) { $restarted = $true } }
            elseif (Done) { $ok = $true; break }
            Start-Sleep 5
        }
    }
    Copy-Item $log "$out\prof_ab_${n}_lite$arm.log" -ErrorAction SilentlyContinue
    $n++
    $ticks = @(); $fps = @(); $mov = @(); $st = 0
    foreach ($l in (Get-Content $log)) {
        if ($l -match '= ([\d.]+) fps .* ([\d]+) us/tick') { $fps += $matches[1]; $ticks += [int]$matches[2] }
        if ($l -match 'traffic (\d+)/20 moving') { $mov += [int]$matches[1] }
        if ($l -like '*lite stuck*') { $st++ }
    }
    Write-Output "arm $n lite=$arm complete=$ok us/tick: $($ticks -join ',') | fps: $($fps -join ',') | moving: $($mov -join ',') | stuck $st"
}
Remove-Item "$w\autodrive.txt" -ErrorAction SilentlyContinue
Set-Content -Path "$w\opts.txt" -Value 'audio 0' -Encoding ascii
foreach ($f in 'autowalk.txt', 'autoinput.txt') {
    if (Test-Path "$w\$f.keep") { Move-Item "$w\$f.keep" "$w\$f" -Force }
}
Get-CimInstance Win32_Process -Filter "Name LIKE 'winuae%'" | Where-Object { $_.CommandLine -match 'gta-prof80\.uae' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force; Write-Output "stopped prof guest $($_.ProcessId)" }
