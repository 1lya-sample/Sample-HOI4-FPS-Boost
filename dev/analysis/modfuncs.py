"""Which exported functions of a system DLL a process spends time in (xperf dump on stdin).
usage: xperf ... | python -I modfuncs.py <pid> <main_tid> <dll name, e.g. ntdll.dll> <path to that dll>
Maps sample addresses to the nearest preceding export (good enough for ntdll/kernelbase).
"""
import sys, struct, bisect, collections

pid, mtid, dllname, dllpath = sys.argv[1], sys.argv[2], sys.argv[3].lower(), sys.argv[4]
d = open(dllpath, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; osz = struct.unpack_from('<H', d, pe + 20)[0]
opt = pe + 24
erva, esz = struct.unpack_from('<II', d, opt + 112)
secs = []; so = opt + osz
for _ in range(nsec):
    vs, va, rs, rp = struct.unpack_from('<IIII', d, so + 8); secs.append((va, max(vs, rs), rp)); so += 40
off = lambda r: next(r - va + rp for va, sz, rp in secs if va <= r < va + sz)
eo = off(erva)
nfun, nnames, afun, aname, aord = struct.unpack_from('<IIIII', d, eo + 20)
funcs = [struct.unpack_from('<I', d, off(afun) + 4 * i)[0] for i in range(nfun)]
exports = {}
for i in range(nnames):
    nm_rva = struct.unpack_from('<I', d, off(aname) + 4 * i)[0]
    o = struct.unpack_from('<H', d, off(aord) + 2 * i)[0]
    nm = d[off(nm_rva):d.index(b'\0', off(nm_rva))].decode()
    r = funcs[o]
    if not (erva <= r < erva + esz):
        exports.setdefault(r, nm)
starts = sorted(exports)

tag = '(%s)' % pid
base = None
cnt = {'main': collections.Counter(), 'worker': collections.Counter()}
for line in sys.stdin:
    s = line.lstrip()
    if s.startswith('SampledProfile,'):
        if tag not in s.replace(' ', ''):
            continue
        p = [x.strip() for x in s.split(',')]
        if p[7].split('!')[0].lower() != dllname:
            continue
        cnt['main' if p[3] == mtid else 'worker'][int(p[4], 16)] += 1
    elif base is None and s.startswith('I-DCStart,'):
        p = [x.strip() for x in s.split(',')]
        if len(p) > 8 and p[8].strip('"').lower().endswith('\\system32\\' + dllname):
            base = int(p[3], 16)
print('base', hex(base) if base else None)
for cls, c in cnt.items():
    agg = collections.Counter()
    for ip, k in c.items():
        i = bisect.bisect_right(starts, ip - base) - 1
        agg[exports[starts[i]] if i >= 0 else '?'] += k
    tot = sum(agg.values())
    print(f'== {cls}: {tot} samples in {dllname}')
    for nm, k in agg.most_common(15):
        print(f'  {100*k/tot:5.1f}% {nm}')
