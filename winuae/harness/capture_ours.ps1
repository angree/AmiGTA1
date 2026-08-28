# Screenshot OUR WinUAE window - the one started with one of our .uae configs.
#
# WHY: the older capture scripts take the first WinUAE window they find
# (Select-Object -First 1). The user runs other Amiga machines from the SAME
# winuae.exe, so that grabbed a StarCraft session instead of the game under
# test - and the screenshot looked like a perfectly healthy Amiga, just not
# ours. Pick the window by PROCESS ID, and get the process by command line.
param(
    [Parameter(Mandatory=$true)][string]$Out,
    [string]$Config = ""
)

$pattern = if ($Config -ne "") { [regex]::Escape($Config) } else { "gta-[a-z0-9-]*\.uae" }

$proc = Get-CimInstance Win32_Process -Filter "Name LIKE 'winuae%.exe'" |
        Where-Object { $_.CommandLine -match $pattern } |
        Select-Object -First 1

if (-not $proc) { Write-Output "ERROR: no WinUAE of ours is running (pattern: $pattern)"; exit 1 }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class W {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  public struct R { public int L, T, Rr, B; }
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Auto)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  delegate bool EnumProc(IntPtr h, IntPtr p);
  // All visible top-level windows of one process, as "handle|title" strings.
  // WinUAE started with -log has TWO: the emulation window (title starts with
  // "WinUAE") and the log console (title is the exe path). MainWindowHandle
  // picks whichever got focus last, which silently screenshots the log.
  public static string[] Windows(uint want) {
    List<string> found = new List<string>();
    EnumWindows(delegate(IntPtr h, IntPtr p) {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid == want && IsWindowVisible(h)) {
        StringBuilder sb = new StringBuilder(512);
        GetWindowText(h, sb, sb.Capacity);
        found.Add(h.ToInt64() + "|" + sb.ToString());
      }
      return true;
    }, IntPtr.Zero);
    return found.ToArray();
  }
}
"@

# The emulation window's title starts with "WinUAE"; the -log console's is the
# exe path. Fall back to MainWindowHandle only if the scan finds nothing.
$wins = [W]::Windows([uint32]$proc.ProcessId)
$emu  = $wins | Where-Object { ($_ -split '\|', 2)[1] -match '^WinUAE' } | Select-Object -First 1
if ($emu) { $h = [IntPtr][int64]($emu -split '\|', 2)[0] }
else      { $h = (Get-Process -Id $proc.ProcessId).MainWindowHandle }
if ($h -eq 0) { Write-Output "ERROR: pid $($proc.ProcessId) has no window yet"; exit 1 }

$r = New-Object W+R
[void][W]::GetWindowRect($h, [ref]$r)
$w = $r.Rr - $r.L; $ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[void][W]::PrintWindow($h, $dc, 2)
$g.ReleaseHdc($dc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "OK saved $Out (${w}x${ht}) from pid $($proc.ProcessId)"
