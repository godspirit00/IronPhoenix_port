"""Linear-sweep scan for fs: accesses, port I/O, privileged instructions, MMIO constants."""
import sys, collections
from capstone import *
sys.path.insert(0, 'tools')
from xbe import Xbe
x = Xbe('extracted/default.xbe')
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.skipdata = True
hits = collections.defaultdict(list)
CODE = ['.text','D3D','D3DX','XGRPH','DSOUND','WMADEC','XMV','XACTENG','XNET','XONLINE','XPP','DOLBY','XON_RD']
for s in x.sections:
    if s.name not in CODE: continue
    code = x.data[s.raw:s.raw+s.rawsize]
    for ins in md.disasm(code, s.vaddr):
        b = ins.bytes
        m = ins.mnemonic
        if b'\x64' in b[:3] and 'fs:' in ins.op_str:
            hits['fs'].append((s.name, ins.address, m, ins.op_str))
        elif m in ('in','out','insb','insd','outsb','outsd','cli','sti','hlt','wrmsr','rdmsr','lidt','lgdt','invlpg','wbinvd','mov') and (m!='mov' or 'cr' in ins.op_str.split(',')[0] or ' cr' in ins.op_str or 'dr' in ins.op_str.split(',')[0][:3]):
            hits['priv'].append((s.name, ins.address, m, ins.op_str))
        if ('0xfd0' in ins.op_str or '0xfe8' in ins.op_str or '0xfec' in ins.op_str or '0xfef' in ins.op_str):
            hits['mmio'].append((s.name, ins.address, m, ins.op_str))
for k, v in hits.items():
    print(f'== {k}: {len(v)}')
    c = collections.Counter((s, m, o) for s, a, m, o in v)
    for (s, m, o), n in c.most_common(60):
        addrs = [hex(a) for ss, a, mm, oo in v if (ss, mm, oo) == (s, m, o)][:4]
        print(f'  {n:4d} {s:8s} {m} {o}   {" ".join(addrs)}')
