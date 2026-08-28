# One anchored A/B series on the -80% machine: every arm runs back to back,
# so the host's drifting "fastest possible" speed divides out. The FIRST arm
# is always the 48h anchor build (23.08, known 18-19 fps when the host is
# quiet) - every later arm is judged against the anchor OF ITS OWN SERIES,
# never against numbers from another hour. The anchor needs the v3 tile set,
# the current build v4; both live pre-staged in C:\temp\amiga_gta_prof.
param(
  [int]$Collect = 45,
  [int]$Timeout = 900
)
$root = "C:\temp\amiga_gta_prof"
$work = "$root\work"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Arm([string]$name, [string]$bin, [string]$til, [string]$opts) {
  Copy-Item $bin "$work\gta-aga" -Force
  Copy-Item $til "$work\GTADATA\style001.til" -Force
  if ($opts -eq "") { Remove-Item "$work\opts.txt" -ErrorAction SilentlyContinue }
  else { Set-Content "$work\opts.txt" $opts -Encoding ASCII }
  $out = & "$here\run-prof.ps1" -Config gta-prof80.uae -Collect $Collect -TimeoutSec $Timeout -WaitFor "gta: tickprof" 2>&1
  "=== $name ==="
  $out | Select-String -Pattern "tickprof|gta: profile" | Select-Object -First 6 | ForEach-Object { $_.Line }
}

Arm "ANCHOR 48h (23.08)" "$root\gta-aga-48h" "$root\work48.til" ""
Arm "CURRENT w256" "I:\GITHUB\Amiga_GTA\build\gta-aga" "$root\til-v4.til" "overlay 1`ntraffic 1`nbenchframes 5`nwidth 256"
Arm "CURRENT w320" "I:\GITHUB\Amiga_GTA\build\gta-aga" "$root\til-v4.til" "overlay 1`ntraffic 1`nbenchframes 5`nwidth 320"
