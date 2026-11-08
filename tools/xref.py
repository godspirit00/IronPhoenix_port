"""Call/reference finder for default.xbe (linear sweep of the code sections).

Usage:
  xref.py calls <va>        call sites of a function (direct calls and calls through [va])
  xref.py refs <va>         instructions whose operands mention the value <va>
  xref.py func <va>         best-guess start of the function containing <va> (prologue scan back)

Linear sweep can mis-decode data inside code sections, so treat results as leads.
The sweep result is cached in tools/__pycache__/xref_cache.pkl (keyed on XBE size).
"""
import os
import pickle
import sys

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

sys.path.insert(0, os.path.dirname(__file__))
from xbe import Xbe

HERE = os.path.dirname(__file__)
XBE_PATH = os.path.join(HERE, '..', 'extracted', 'default.xbe')
CACHE = os.path.join(HERE, '__pycache__', 'xref_cache.pkl')
CODE_SECTIONS = ('.text', 'D3D', 'D3DX', 'XGRPH', 'DSOUND', 'WMADEC', 'XMV', 'XACTENG',
                 'XNET', 'XONLINE', 'XPP')


def sweep(x):
    """Return list of (address, mnemonic, op_str) for every decodable instruction."""
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.skipdata = True
    out = []
    for s in x.sections:
        if s.name not in CODE_SECTIONS:
            continue
        data = x.data[s.raw:s.raw + s.rawsize]
        for ins in md.disasm_lite(data, s.vaddr):
            addr, size, mnem, ops = ins
            out.append((addr, mnem, ops))
    return out


def load():
    x = Xbe(XBE_PATH)
    key = len(x.data)
    if os.path.exists(CACHE):
        with open(CACHE, 'rb') as f:
            k, insns = pickle.load(f)
        if k == key:
            return x, insns
    insns = sweep(x)
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    with open(CACHE, 'wb') as f:
        pickle.dump((key, insns), f)
    return x, insns


def main():
    cmd, va = sys.argv[1], int(sys.argv[2], 16)
    x, insns = load()
    h = '%#x' % va
    if cmd == 'calls':
        for a, m, o in insns:
            if m == 'call' and (o == h or o == 'dword ptr [%s]' % h):
                print('%08x  %s %s' % (a, m, o))
    elif cmd == 'refs':
        for a, m, o in insns:
            if h in o.replace(',', ' ').replace('[', ' ').replace(']', ' ').split():
                print('%08x  %s %s' % (a, m, o))
    elif cmd == 'func':
        # Walk back to the nearest 'push ebp; mov ebp, esp' or an instruction after ret/int3 padding.
        idx = max(i for i, (a, _, _) in enumerate(insns) if a <= va)
        for i in range(idx, max(0, idx - 4000), -1):
            a, m, o = insns[i]
            pm = insns[i - 1][1] if i else ''
            if (m == 'push' and o == 'ebp' and insns[i + 1][1:] == ('mov', 'ebp, esp')) or pm in ('ret', 'int3'):
                print('%08x' % a)
                return
        print('not found')


if __name__ == '__main__':
    main()
