param([int]$WinPid = 0)
# Put OUR minimised gta-*.uae WinUAE window back on screen WITHOUT giving it
# the focus (SW_SHOWNOACTIVATE). A minimised WinUAE 2.8.1 is PAUSED - the
# game stops logging and every reload waits for nothing (PROGRESS 133 s.3).
# Matches the process by its command line, like kill_ours.ps1, and touches
# no other emulator. Never activates, never sends input.
Add-Type -Name WinShow2 -Namespace GtaHarness -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int c);
[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint wp);
[DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
'@
$ours = @(Get-CimInstance Win32_Process -Filter "name='winuae.exe'" |
          Where-Object { $_.CommandLine -match 'gta-[^\\/]*\.uae' } |
          ForEach-Object { [uint32]$_.ProcessId })
# capture_ours.ps1 prints the pid it found; pass it when the process query
# comes back empty (it does on this machine without elevation)
if ($WinPid -gt 0) { $ours = @([uint32]$WinPid) }
if ($ours.Count -eq 0) { Write-Output "no gta-*.uae WinUAE running"; exit 1 }
$cb = [GtaHarness.WinShow2+EnumProc]{
  param($h, $l)
  [uint32]$wp = 0
  [GtaHarness.WinShow2]::GetWindowThreadProcessId($h, [ref]$wp) | Out-Null
  if ($ours -contains $wp) {
    $cn = New-Object System.Text.StringBuilder 64
    [GtaHarness.WinShow2]::GetClassName($h, $cn, 64) | Out-Null
    if ($cn.ToString() -eq 'PCsuxRox' -and [GtaHarness.WinShow2]::IsIconic($h)) {
      [GtaHarness.WinShow2]::ShowWindowAsync($h, 4) | Out-Null
      Write-Output "restored (no activate) pid $wp"
    }
  }
  return $true
}
[GtaHarness.WinShow2]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
