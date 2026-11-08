"""Disassemble default.xbe at VA. Usage: dis.py <va> [count]"""
import sys
from capstone import *
sys.path.insert(0, __import__('os').path.dirname(__file__))
from xbe import Xbe
x = Xbe(__import__('os').path.join(__import__('os').path.dirname(__file__), '..', 'extracted', 'default.xbe'))
md = Cs(CS_ARCH_X86, CS_MODE_32)
va = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
for i, ins in enumerate(md.disasm(x.read(va, n * 15), va)):
    if i >= n: break
    print(f'{ins.address:08x}  {ins.bytes.hex():20s} {ins.mnemonic} {ins.op_str}')
