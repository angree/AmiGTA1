# Same scenario, several binaries, one running emulator: which build lost the
# speed? Each arm is copied over Work:AmiGTA and started with reload-gta.ps1 -
# no emulator restart. The current binary is put back at the end and every arm's
# log is kept in C:\temp\amiga_gta\ab\. Interleave an anchor arm: emulated fps
# drift with the host.
#
#   ab-bins.ps1 -Arms now,v0.6.1,now -Ticks 1500
#   ab-bins.ps1 -Arms now,x=C:\path\to\AmiGTA,now -Script tools\scripts\foo.txt
param(
  [string[]]$Arms = @('now', 'v0.6.1', 'v0.4.0', 'v0.2.0', 'now'),
  [int]$Ticks = 1500,
  [string]$Script = '',
  [int]$TimeoutSec = 400
)
$work = 'C:\temp\amiga_gta\work'
$out = 'C:\temp\amiga_gta\ab'
$now = 'C:\temp\amiga_gta_build\build\AmiGTA'
New-Item -ItemType Directory -Force $out | Out-Null
$reload = Join-Path $PSScriptRoot 'reload-gta.ps1'
$n = 0
$Arms = @($Arms | ForEach-Object { $_ -split "," })
foreach ($a in $Arms) {
  $n++
  $label = $a; $bin = $null
  if ($a -match '^([^=]+)=(.+)$') { $label = $Matches[1]; $bin = $Matches[2] }
  elseif ($a -eq 'now') { $bin = $now }
  else { $bin = "C:\temp\amiga_gta\oldbins\$a\AmiGTA.bin" }
  Copy-Item $bin "$work\AmiGTA" -Force
  Set-Content "$work\startlevel.txt" '0' -Encoding ascii
  if ($Script -ne '') { Copy-Item $Script "$work\autodrive.txt" -Force }
  else { @("wait $Ticks") | Set-Content "$work\autodrive.txt" -Encoding ascii }
  powershell -ExecutionPolicy Bypass -File $reload -WaitFor 'autodrive done' -TimeoutSec $TimeoutSec | Out-Null
  $log = "$out\$n-$label.log"
  Copy-Item "$work\gta.log" $log -Force
  $rows = Select-String -Path $log -Pattern 'profile 100 frames (\d+) us = ([\d.]+) fps \| sim (\d+) us/f \(([\d.]+) ticks/f, (\d+) us/tick\) \| render (\d+) \| present (\d+) \(c2p (\d+)\) \| cols (\d+) walls (\d+) spr (\d+) \| w(\d+) z(\d+)'
  if (-not $rows) { "{0,-10} no profile lines" -f $label; continue }
  $r = @($rows | Select-Object -Skip 1 | ForEach-Object { $_.Matches[0] })
  if ($r.Count -eq 0) { $r = @($rows | ForEach-Object { $_.Matches[0] }) }
  function Mean($i) { [math]::Round(($r | ForEach-Object { [double]$_.Groups[$i].Value } | Measure-Object -Average).Average, 1) }
  "{0,-10} n={1,-3} fps {2,5} | sim/f {3,6} ticks/f {4,4} tick {5,6} | render {6,6} | present {7,6} c2p {8,6} | cols {9,5} walls {10,4} spr {11,3} w{12} z{13}" -f $label, @($r).Count, (Mean 2), (Mean 3), (Mean 4), (Mean 5), (Mean 6), (Mean 7), (Mean 8), (Mean 9), (Mean 10), (Mean 11), (Mean 12), (Mean 13)
}
Copy-Item $now "$work\AmiGTA" -Force
