# GTA's OWN MorphOS 3.20 QEMU LAB (E:\morphos_gta), so no other session's
# QEMU is touched: own qemu exe name (qemu-gta-ppc.exe), own monitor port
# 4557, own xfer\ (the guest's ide0:) and shots\. The MorphOS ISO and boot.img
# are read from E:\morphos_emu, read-only. See
# I:\GITHUB\!instrukcje_kompilacji_68K_AROS_i_MorphOS\40-MorphOS-zgodnosc-68k.md.
$e = 'E:\morphos_emu'; $d = 'E:\morphos_gta'
New-Item -ItemType Directory -Force "$d\xfer", "$d\shots" | Out-Null
if (-not (Test-Path "$d\qemu\qemu-gta-ppc.exe")) {
    robocopy 'E:\morphos_ttd\qemu' "$d\qemu" /E /NFL /NDL /NJH /NJS /NP | Out-Null
    Rename-Item "$d\qemu\qemu-ttd-ppc.exe" 'qemu-gta-ppc.exe'
}
foreach ($f in 'mon.ps1', 'mon-multi.ps1', 'type.ps1') {
    (Get-Content "$e\$f" -Raw) -replace '4555', '4557' -replace 'E:\\morphos_emu', 'E:\morphos_gta' |
        Set-Content "$d\$f" -Encoding ascii
}
@'
# GTA's own MorphOS QEMU (pegasos2), no window, monitor tcp 4557.
param([string]$Iso = 'E:\morphos_gta\gta-game.iso')
$e = 'E:\morphos_emu'; $d = 'E:\morphos_gta'
$a = @(
  '-machine', 'pegasos2', '-m', '1024', '-rtc', 'base=localtime',
  '-device', 'ati-vga,guest_hwcursor=true,romfile=',
  '-kernel', "$e\boot.img",
  '-drive', "file=$e\morphos-3.20.iso,if=ide,index=2,media=cdrom,readonly=on",
  '-drive', "file=$Iso,if=ide,index=3,media=cdrom,readonly=on",
  '-drive', "file=fat:16:rw:E:/morphos_gta/xfer,format=raw,if=ide,index=0",
  '-display', 'none', '-audio', 'none',
  '-monitor', 'tcp:127.0.0.1:4557,server,nowait',
  '-serial', "file:$d\serial.log"
)
$p = Start-Process -FilePath "$d\qemu\qemu-gta-ppc.exe" -ArgumentList $a -WindowStyle Hidden -PassThru
Set-Content "$d\qemu.pid" $p.Id
"qemu pid $($p.Id)"
'@ | Set-Content "$d\start-mos.ps1" -Encoding ascii
"lab ready: $d"
