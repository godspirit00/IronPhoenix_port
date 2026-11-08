"""Extract kernel export signatures (name, callconv, nargs, return type) from Cxbx headers."""
import re, glob, json
sigs = {}
for f in glob.glob('third_party/Cxbx-Reloaded/src/core/kernel/common/*.h'):
    txt = open(f, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'XBSYSAPI\s+EXPORTNUM\((\d+)\)\s+([\w\s\*]+?)\s+(NTAPI|FASTCALL|CDECL|_cdecl|__cdecl|DECLSPEC_NORETURN NTAPI)?\s*(\w+)\s*(\([^;]*?\))?\s*;', txt):
        num = int(m.group(1)); ret = m.group(2).strip(); cc = m.group(3) or ''; name = m.group(4); args = m.group(5)
        if args is None:
            sigs[num] = dict(name=name, kind='var', type=ret)
            continue
        a = args.strip()[1:-1]
        a = re.sub(r'/\*.*?\*/', '', a, flags=re.S); a = re.sub(r'//[^\n]*', '', a)
        parts = [p.strip() for p in a.split(',') if p.strip()]
        if parts == ['void'] or parts == ['VOID']: parts = []
        sigs[num] = dict(name=name, kind='func', ret=ret, cc=cc, args=parts)
json.dump(sigs, open('notes/kernel_sigs.json', 'w'), indent=1)
imps = [int(x) for x in open('notes/xbe_info.txt').read().split('kernel imports (145):')[1].split()]
missing = [i for i in imps if i not in sigs]
print(len(sigs), 'sigs; imports missing sigs:', missing)
for i in imps:
    s = sigs.get(i)
    if s and s['kind'] == 'func' and s['cc'] not in ('NTAPI',):
        print(i, s['name'], s['cc'], len(s['args']), s['args'])
