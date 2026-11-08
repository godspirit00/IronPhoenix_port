"""Find fs:[disp] instructions (disp != 0, 4, 8, 0x18 which match Windows TEB semantics)
in real code and emit a C table for runtime patching."""
import sys, os
from capstone import *
from capstone.x86 import *
sys.path.insert(0, os.path.dirname(__file__))
from xbe import Xbe
x = Xbe(os.path.join(os.path.dirname(__file__), '..', 'extracted', 'default.xbe'))
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
CODE = ['.text', 'D3D', 'DSOUND', 'XACTENG', 'XNET', 'XONLINE', 'XPP', 'XMV', 'WMADEC', 'XGRPH', 'D3DX']
SAFE = {0}
out = []
for s in x.sections:
    if s.name not in CODE: continue
    data = x.data[s.raw:s.raw + s.rawsize]
    pos = 0
    while True:
        pos = data.find(b'\x64', pos)
        if pos < 0: break
        va = s.vaddr + pos
        ins = next(md.disasm(data[pos:pos + 16], va), None)
        pos += 1
        if ins is None: continue
        mem = [o for o in ins.operands if o.type == X86_OP_MEM]
        if not mem or mem[0].mem.segment != X86_REG_FS: continue
        m = mem[0].mem
        if m.base or m.index or m.disp in SAFE or m.disp > 0x1000: continue
        # Require that the preceding bytes decode cleanly into this instruction (sanity: look back 32 bytes)
        ok = False
        for back in range(1, 40):
            st = va - back
            if st < s.vaddr: break
            seq = list(md.disasm(x.read(st, back + ins.size), st))
            addrs = [i.address for i in seq]
            if va in addrs and sum(i.size for i in seq if i.address < va) == back:
                ok = True; break
        if not ok: continue
        out.append((va, ins.size, ins.mnemonic, ins.op_str, ins.bytes.hex(), s.name))
for o in out:
    print(f'{o[0]:#x} {o[1]} {o[5]:8s} {o[2]} {o[3]}   [{o[4]}]')
