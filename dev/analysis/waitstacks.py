"""Where does the main thread block? Groups the main thread's context switches out (by wait reason)
by the game functions on the stack captured at that moment (xperf dump on stdin).
usage: xperf ... | python -I waitstacks.py <pid> <main_tid> <exe>
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
waits = {}                 # ts -> [reason, wait-time-of-next-run unknown, exe ips, other modules]
last_switch_out = {}       # for measuring how long the main thread stayed off the CPU
blocked_ms = collections.Counter()
pending_out = None
for line in sys.stdin:
    s = line.lstrip()
    if s.startswith('CSwitch,'):
        p = [x.strip() for x in s.split(',')]
        if p[1] == 'TimeStamp':
            continue
        ts = int(p[1])
        new_tid, old_tid = p[3], p[9]
        if old_tid == mtid and p[8].replace(' ', '').endswith(tag):
            waits[p[1]] = [p[13], [], []]
            pending_out = (ts, p[1])
        if new_tid == mtid and pending_out:
            out_ts, key = pending_out
            if key in waits:
                waits[key].append((ts - out_ts) / 1000.0)    # ms off the CPU
            pending_out = None
    elif s.startswith('Stack,'):
        p = [x.strip() for x in s.split(',')]
        if p[1] in waits and p[2] == mtid:
            img = p[5].split('!')[0].lower()
            if img == 'hoi4.exe':
                waits[p[1]][1].append(int(p[4], 16))
            elif len(waits[p[1]][2]) < 6:
                waits[p[1]][2].append(p[5].split('!')[0])
    elif base is None and s.startswith('I-DCStart,') and tag in s.replace(' ', ''):
        p = [x.strip() for x in s.split(',')]
        if p[8].strip('"').lower().endswith('hoi4.exe'):
            base = int(p[3], 16)

by = collections.defaultdict(lambda: [0, 0.0])
reasons = collections.defaultdict(lambda: [0, 0.0])
for key, w in waits.items():
    reason, ips, mods = w[0], w[1], w[2]
    off_ms = w[3] if len(w) > 3 else 0.0
    reasons[reason][0] += 1; reasons[reason][1] += off_ms
    fs = []
    for ip in ips:
        f = fstart(ip - base)
        if f and (not fs or fs[-1] != f):
            fs.append(f)
        if len(fs) == 4:
            break
    k = (reason, ' <- '.join('fn_%x' % f for f in fs) or '(no game frame) ' + '/'.join(mods[:3]))
    by[k][0] += 1; by[k][1] += off_ms
print('main thread time off the CPU by wait reason (ms):')
for r, (n, ms) in sorted(reasons.items(), key=lambda x: -x[1][1])[:8]:
    print(f'  {r:18} {n:6} switches {ms:9.0f} ms')
print('top blocking places (by time off the CPU):')
for (r, k), (n, ms) in sorted(by.items(), key=lambda x: -x[1][1])[:14]:
    print(f'  {ms:8.0f} ms {n:5}x {r:14} {k}')
