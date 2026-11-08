"""XBE parser. Usage: xbe.py <file.xbe>"""
import struct, sys

class Section:
    pass

class Xbe:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        assert d[:4] == b'XBEH'
        (self.base, self.hdr_size, self.img_size, self.img_hdr_size, self.timestamp,
         self.cert_addr, self.nsect, self.sect_addr, self.init_flags, ep, self.tls_addr,
         self.stack_size, self.heap_reserve, self.heap_commit, self.pe_base, self.pe_size,
         self.pe_checksum, self.pe_timestamp, self.dbg_path, self.dbg_file, self.dbg_ufile,
         kt, self.nonkernel_imports, self.nlibs, self.libs_addr, self.klib_addr,
         self.xapilib_addr, self.logo_addr, self.logo_size) = struct.unpack_from('<29I', d, 0x104)
        # Retail vs debug XOR keys
        for epk, ktk, kind in ((0xA8FC57AB, 0x5B6D40B6, 'retail'), (0x94859D4B, 0xEFB1F152, 'debug')):
            e, k = ep ^ epk, kt ^ ktk
            if self.base <= e < self.base + self.img_size:
                self.entry, self.kthunk, self.kind = e, k, kind
                break
        self.sections = []
        for i in range(self.nsect):
            o = self.sect_addr - self.base + i * 56
            s = Section()
            (s.flags, s.vaddr, s.vsize, s.raw, s.rawsize, s.name_addr,
             s.refcount, s.head_ref, s.tail_ref) = struct.unpack_from('<9I', d, o)
            s.name = self.cstr(s.name_addr)
            self.sections.append(s)
        self.libs = []
        for i in range(self.nlibs):
            o = self.libs_addr - self.base + i * 16
            name = d[o:o + 8].rstrip(b'\0').decode()
            maj, mnr, bld, fl = struct.unpack_from('<4H', d, o + 8)
            self.libs.append((name, maj, mnr, bld, fl))

    def hdr_off(self, va):
        return va - self.base

    def cstr(self, va):
        o = self.hdr_off(va)
        return self.data[o:self.data.index(b'\0', o)].decode('latin1')

    def va_to_file(self, va):
        for s in self.sections:
            if s.vaddr <= va < s.vaddr + s.rawsize:
                return s.raw + va - s.vaddr
        if va < self.base + self.hdr_size:
            return va - self.base
        return None

    def read(self, va, n):
        o = self.va_to_file(va)
        return self.data[o:o + n]

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def kernel_imports(self):
        out = []
        va = self.kthunk
        while True:
            v = self.u32(va)
            if v == 0:
                break
            out.append((va, v & 0x7FFFFFFF))
            va += 4
        return out

if __name__ == '__main__':
    x = Xbe(sys.argv[1])
    print(f'kind={x.kind} base={x.base:#x} imgsize={x.img_size:#x} entry={x.entry:#x} '
          f'kthunk={x.kthunk:#x} tls={x.tls_addr:#x} stack={x.stack_size:#x} initflags={x.init_flags:#x}')
    print('debug path:', x.cstr(x.dbg_path))
    print('sections:')
    for s in x.sections:
        print(f'  {s.name:10s} va={s.vaddr:#010x} vsize={s.vsize:#09x} raw={s.raw:#09x} rawsize={s.rawsize:#09x} flags={s.flags:#x}')
    print('libraries:')
    for l in x.libs:
        print(f'  {l[0]:8s} {l[1]}.{l[2]}.{l[3]} flags={l[4]:#x}')
    ki = x.kernel_imports()
    print(f'kernel imports ({len(ki)}):', ' '.join(str(o) for _, o in ki))
