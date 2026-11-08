"""XDVDFS (XISO) lister/extractor. Usage: xiso.py <iso> [outdir] [--only name]"""
import struct, sys, os

SECTOR = 2048

def read_dir(f, base, sector, size, prefix, out):
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, fsize, attr, nlen = struct.unpack_from('<HHIIBB', data, off)
        if left == 0xFFFF:
            continue
        name = data[off + 14: off + 14 + nlen].decode('latin1')
        path = prefix + '/' + name if prefix else name
        if attr & 0x10:
            if fsize:
                read_dir(f, base, start, fsize, path, out)
            out.append((path, None, 0))
        else:
            out.append((path, start, fsize))
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)

def list_iso(path):
    f = open(path, 'rb')
    for base in (0, 0x18300000, 0xFD90000, 0x30600000):
        f.seek(base + 32 * SECTOR)
        if f.read(20) == b'MICROSOFT*XBOX*MEDIA':
            break
    else:
        raise SystemExit('no XDVDFS header')
    root_sector, root_size = struct.unpack('<II', f.read(8))
    out = []
    read_dir(f, base, root_sector, root_size, '', out)
    return f, base, out

if __name__ == '__main__':
    f, base, entries = list_iso(sys.argv[1])
    outdir = sys.argv[2] if len(sys.argv) > 2 else None
    only = sys.argv[sys.argv.index('--only') + 1] if '--only' in sys.argv else None
    for path, sec, size in sorted(entries):
        if sec is None:
            continue
        if outdir is None:
            print(f'{size:12d}  {path}')
            continue
        if only and only.lower() != path.lower():
            continue
        dst = os.path.join(outdir, path)
        os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
        f.seek(base + sec * SECTOR)
        with open(dst, 'wb') as o:
            remaining = size
            while remaining:
                chunk = f.read(min(remaining, 1 << 22))
                o.write(chunk)
                remaining -= len(chunk)
        print('extracted', path)
