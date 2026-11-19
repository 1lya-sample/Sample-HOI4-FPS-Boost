"""For main-thread samples that are in OS code (ntdll / kernel / KernelBase), which game function
called into the OS? Groups by the first hoi4.exe frame on the stack (= the waiting game code).
usage: xperf ... | python -I oscallers.py <pid> <main_tid> <exe>
"""
import sys, struct, bisect, collections

pid, mtid, exe = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; osz = struct.unpack_from('<H', d, pe + 20)[0]
opt = pe + 24
exc_rva, exc_size = struct.unpack_from('<II', d, opt + 112 + 24)
secs = []; so = opt + osz
for _ in range(nsec):
    vs, va, rs, rp = struct.unpack_from('<IIII', d, so + 8); secs.append((va, max(vs, rs), rp)); so += 40
off = lambda r: next(r - va + rp for va, sz, rp in secs if va <= r < va + sz)
po = off(exc_rva); funcs = {}
for i in range(exc_size // 12):
    b, e, u = struct.unpack_from('<III', d, po + i * 12)
    if b:
        funcs[b] = e
starts = sorted(funcs)


def fstart(rva):
    i = bisect.bisect_right(starts, rva) - 1
    return starts[i] if i >= 0 and rva < funcs[starts[i]] else None


tag = '(%s)' % pid
base = None
os_samples = {}   # ts -> [os function image, first exe frame or None, second exe frame]
cur = None
for line in sys.stdin:
    s = line.lstrip()
    if s.startswith('SampledProfile,'):
        p = [x.strip() for x in s.split(',')]
        if p[1] == 'TimeStamp' or p[3] != mtid or not p[2].replace(' ', '').endswith(tag):
            continue
        img = p[7].split('!')[0].lower()
        if img in ('ntdll.dll', 'ntoskrnl.exe', 'kernelbase.dll', 'kernel32.dll'):
            os_samples[p[1]] = [img, []]
    elif s.startswith('Stack,'):
        p = [x.strip() for x in s.split(',')]
        if p[1] in os_samples and p[2] == mtid and p[5].lower().startswith('hoi4.exe'):
            os_samples[p[1]][1].append(int(p[4], 16))
    elif base is None and s.startswith('I-DCStart,') and tag in s.replace(' ', ''):
        p = [x.strip() for x in s.split(',')]
        if p[8].strip('"').lower().endswith('hoi4.exe'):
            base = int(p[3], 16)

by = collections.Counter()
for ts, (img, ips) in os_samples.items():
    fs = [fstart(ip - base) for ip in ips[:3]]
    key = ' <- '.join('fn_%x' % f for f in fs if f) or '(no game frame)'
    by[(img, key)] += 1
tot = len(os_samples)
print('main samples in OS code:', tot)
for (img, key), c in by.most_common(15):
    print(f'{100*c/tot:5.1f}% {img:13} {key}')
