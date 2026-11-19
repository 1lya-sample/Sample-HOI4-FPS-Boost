"""How busy are worker threads while the main thread is inside a parallel-for wait?
usage: xperf ... | python -I concurrency.py <pid> <main_tid> <exe>

For every 1 ms bucket: is the main thread inside the job-group wait (fn 0x25576b0), and how
many worker threads were sampled running in that bucket (in hoi4.exe code vs. OS code).
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
WAITGROUP = 0x25576b0
INLINE = 0x2559a80
SPIN = 0x25577e0

tag = '(%s)' % pid
base = None
main_state = {}            # ts -> set of exe ips (main stacks)
worker = collections.defaultdict(lambda: [set(), set()])   # bucket ms -> [tids in exe code, tids in os code]
sample_ts = {}
for line in sys.stdin:
    s = line.lstrip()
    if s.startswith('SampledProfile,'):
        p = [x.strip() for x in s.split(',')]
        if p[1] == 'TimeStamp' or not p[2].replace(' ', '').endswith(tag):
            continue
        ts = int(p[1])
        if p[3] == mtid:
            main_state[p[1]] = set()
        else:
            k = 0 if p[7].lower().startswith('hoi4.exe') else 1
            worker[ts // 1000][k].add(p[3])
    elif s.startswith('Stack,'):
        p = [x.strip() for x in s.split(',')]
        if p[1] in main_state and p[2] == mtid and p[5].lower().startswith('hoi4.exe'):
            main_state[p[1]].add(int(p[4], 16))
    elif base is None and s.startswith('I-DCStart,') and tag in s.replace(' ', ''):
        p = [x.strip() for x in s.split(',')]
        if p[8].strip('"').lower().endswith('hoi4.exe'):
            base = int(p[3], 16)


def fstart(rva):
    i = bisect.bisect_right(starts, rva) - 1
    return starts[i] if i >= 0 and rva < funcs[starts[i]] else None


stats = collections.defaultdict(lambda: [0, 0, 0, collections.Counter()])
for ts, ips in main_state.items():
    fs = {fstart(ip - base) for ip in ips}
    if WAITGROUP in fs:
        st = 'pfor-inline' if INLINE in fs else ('pfor-spin' if SPIN in fs else 'pfor-other')
    else:
        st = 'serial'
    b = int(ts) // 1000
    w = worker.get(b, [set(), set()])
    a = stats[st]
    a[0] += 1; a[1] += len(w[0]); a[2] += len(w[1]); a[3][len(w[0])] += 1
tot = sum(a[0] for a in stats.values())
for st, (n, we, wo, hist) in sorted(stats.items()):
    print(f'{st:12} {n:6} ms ({100*n/tot:4.1f}% of main)  workers in game code: {we/n:4.2f} avg, in OS code: {wo/n:4.2f} avg;'
          f' hist(0..10): {[hist.get(i, 0) for i in range(11)]}')
