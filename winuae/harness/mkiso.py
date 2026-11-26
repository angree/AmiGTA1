# mkiso.py - the CD image for the MorphOS lab without WSL's genisoimage.
#
#   python mkiso.py <src dir> <out.iso> [volume]
#
# Same shape as `genisoimage -R -J -V GTA`: Rock Ridge (the long, mixed-case
# names MorphOS's CD filesystem shows) plus Joliet, with plain ISO9660 8.3
# names underneath, generated here because pycdlib wants them spelled out.
import os, sys
import pycdlib

src, out = sys.argv[1], sys.argv[2]
vol = sys.argv[3] if len(sys.argv) > 3 else 'GTA'

iso = pycdlib.PyCdlib()
iso.new(interchange_level=3, rock_ridge='1.09', joliet=3, vol_ident=vol)

def iso_name(name, used, is_dir):
    base, ext = os.path.splitext(name)
    ok = lambda s: ''.join(c if c.isalnum() else '_' for c in s.upper())
    base, ext = ok(base)[:8] or '_', ok(ext[1:])[:3]
    n, cand = 0, None
    while True:
        b = base if n == 0 else (base[:8 - len(str(n))] + str(n))
        cand = b if is_dir else (b + '.' + ext + ';1')
        if cand not in used:
            used.add(cand)
            return cand
        n += 1

def walk(host, iso_dir, jol_dir):
    used = set()
    for name in sorted(os.listdir(host)):
        p = os.path.join(host, name)
        isd = os.path.isdir(p)
        n = iso_name(name, used, isd)
        ip = (iso_dir.rstrip('/') + '/' + n)
        jp = (jol_dir.rstrip('/') + '/' + name)
        if isd:
            iso.add_directory(ip, rr_name=name, joliet_path=jp)
            walk(p, ip, jp)
        else:
            # rwx for everyone: the guest copies these to RAM: and must run the
            # binary and overwrite its logs - pycdlib's default is read-only
            iso.add_file(p, ip, rr_name=name, joliet_path=jp, file_mode=0o100777)

walk(src, '/', '/')
iso.write(out)
iso.close()
print('ISO_OK', out)
