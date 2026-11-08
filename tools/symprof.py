"""Summarize build/profile.txt: symbolize host addresses and Xbox addresses, hide idle threads."""
import re, subprocess, bisect, sys
import os
TC = os.path.abspath('toolchain/llvm-mingw-20261006-ucrt-x86_64/bin/llvm-addr2line.exe')
xsyms = []
for l in open('notes/symbols_raw.txt'):
    m = re.search(r'__(\w+?)\(.*= (0x[0-9a-f]+)', l)
    if m: xsyms.append((int(m.group(2), 16), m.group(1)))
xsyms.sort()
threads = []
sysn = {}
cur = None
for l in open('build/profile.txt'):
    m = re.match(r'thread (\d+): (\d+) samples', l)
    if m:
        cur = [int(m.group(1)), int(m.group(2)), []]; threads.append(cur); continue
    parts = l.split(); a, n = parts[0], parts[1]
    if len(parts) > 2 and parts[2].startswith('<direct'):
        cur.append((int(a, 16), -int(n))); sysn[int(a, 16)] = parts[3] if len(parts) > 3 else ''; continue
    if len(parts) > 2 and parts[2].startswith('<caller'):
        cur.append((int(a, 16), int(n))); continue
    cur[2].append((int(a, 16), int(n))); sysn[int(a, 16)] = parts[2] if len(parts) > 2 else ''
host = sorted({a for t in threads for a, n in t[2] + t[3:] if 0x4b0000 <= a < 0x1000000})
names = {}
if host:
    out = subprocess.run([TC, '-f', '-C', '-e', 'build/ironphoenix.exe'] + [hex(a) for a in host], capture_output=True, text=True).stdout.split('\n')
    for i, a in enumerate(host): names[a] = out[2 * i]
def name(a):
    if a in names: return 'host:' + names[a][:60]
    if 0x11000 <= a < 0x4b0000:
        i = bisect.bisect_right([s[0] for s in xsyms], a) - 1
        near = xsyms[i] if i >= 0 else None
        if near and a - near[0] < 0x800: return 'xbox:%s+%x' % (near[1], a - near[0])
        return 'xbox:%08x' % a
    return 'sys:%08x %s' % (a, sysn.get(a, ''))
for tid, total, rows, *callers in threads:
    busy = sum(n for a, n in rows if not (0x70000000 <= a < 0x80000000))
    if busy < total * 0.02 and total > 50 and not any(0x4b0000 <= a < 0x1000000 for a, n in rows): continue
    print('thread %d: %d samples (%.0f%% outside system DLLs)' % (tid, total, 100.0 * busy / max(total, 1)))
    byfn = {}
    for a, n in rows:
        k = name(a)
        k = re.sub(r'\+[0-9a-f]+$', '', k) if not k.startswith('sys:') else re.sub(r'^sys:[0-9a-f]+ ', 'sys:', re.sub(r'\+[0-9a-f]+$', '', k))
        if k.startswith('xbox:') and re.match(r'xbox:[0-9a-f]{8}$', k): k = 'xbox:%05xxx' % (int(k[5:], 16) >> 8)
        byfn[k] = byfn.get(k, 0) + n
    for k, n in sorted(byfn.items(), key=lambda x: -x[1])[:18]: print('   %5d %s' % (n, k))
    for a, n in callers:
        if n < 0: print('   %5d   (syscall stub returns to %s)' % (-n, name(a)))
        else: print('   %5d   (in system code, called from %s)' % (n, name(a)))
