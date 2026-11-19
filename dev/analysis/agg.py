"""Aggregate an xperf text dump (stdin) for one process.

usage: xperf -i t.etl -tle -tti -quiet | python -I agg.py <pid> <exe_path> <out.json> <main_tid>

Collects for the process: samples per thread and per CPU, self time per module and per
function of the exe (function ranges from the PE .pdata table), inclusive time per function
from sampled stacks, caller->callee edges on the main thread, and context-switch wait reasons.
Also counts samples per CPU for every process and the trace duration.
"""
import sys, struct, json, bisect, collections

PID, EXE, OUT, MAIN_TID = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])


def load_funcs(path):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    optsz = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    exc_rva, exc_size = struct.unpack_from('<II', data, opt + 112 + 3 * 8)
    secs = []
    so = opt + optsz
    for _ in range(nsec):
        vsize, va, rsize, rptr = struct.unpack_from('<IIII', data, so + 8)
        secs.append((va, max(vsize, rsize), rptr))
        so += 40

    def off(rva):
        for va, sz, rp in secs:
            if va <= rva < va + sz:
                return rva - va + rp
        raise ValueError(hex(rva))

    po = off(exc_rva)
    funcs = {}
    for i in range(exc_size // 12):
        b, e, u = struct.unpack_from('<III', data, po + i * 12)
        if b == 0:
            continue
        root, uu = b, u
        for _ in range(32):  # chained unwind info -> primary function
            if uu & 1:
                break
            uo = off(uu)
            if not (data[uo] >> 3) & 4:
                break
            cnt = data[uo + 2]
            cb, ce, cu = struct.unpack_from('<III', data, uo + 4 + 2 * ((cnt + 1) & ~1))
            root, uu = cb, cu
        funcs[b] = (e, root)
    return sorted(funcs), funcs


starts, funcs = load_funcs(EXE)


def rva_to_func(rva):
    i = bisect.bisect_right(starts, rva) - 1
    if i >= 0:
        e, root = funcs[starts[i]]
        if rva < e:
            return root
    return None


tag = '(%s)' % PID
base = None
thread_samples = collections.Counter()
thread_cpu = collections.defaultdict(collections.Counter)
excl_mod = collections.defaultdict(collections.Counter)
raw_excl = collections.defaultdict(collections.Counter)
raw_stacks = collections.defaultdict(list)
stack_count = collections.Counter()
cswitch_wait = collections.defaultdict(collections.Counter)
per_cpu = collections.defaultdict(collections.Counter)
tmin = tmax = None
pending = {}
cur = None  # [cls, list of exe ips]


def flush():
    global cur
    if cur and cur[1]:
        raw_stacks[cur[0]].append(tuple(cur[1]))
    cur = None


n = 0
for line in sys.stdin:
    n += 1
    s = line.lstrip()
    if s.startswith('SampledProfile,'):
        p = [x.strip() for x in s.split(',')]
        if p[1] == 'TimeStamp':
            continue
        ts, cpu = int(p[1]), int(p[5])
        tmin = ts if tmin is None else min(tmin, ts)
        tmax = ts if tmax is None else max(tmax, ts)
        proc = p[2].replace(' ', '')
        tid = int(p[3])
        if not proc.endswith(tag):
            per_cpu[cpu]['idle' if proc.startswith('Idle(') else 'other'] += 1
            continue
        cls = 'main' if tid == MAIN_TID else 'worker'
        per_cpu[cpu][cls] += 1
        thread_samples[tid] += 1
        thread_cpu[tid][cpu] += 1
        mod = p[7].split('!')[0]
        excl_mod[cls][mod] += 1
        if mod.lower() == 'hoi4.exe':
            raw_excl[cls][int(p[4], 16)] += 1
        pending[(p[1], tid)] = cls
        if len(pending) > 20000:
            for k in list(pending)[:10000]:
                del pending[k]
    elif s.startswith('Stack,'):
        p = [x.strip() for x in s.split(',')]
        if p[1] == 'TimeStamp':
            continue
        cls = pending.get((p[1], int(p[2])))
        if cls is None:
            continue
        if int(p[3]) == 1:
            flush()
            cur = [cls, []]
            stack_count[cls] += 1
        if cur is not None and p[5].lower().startswith('hoi4.exe'):
            cur[1].append(int(p[4], 16))
    elif s.startswith('I-DCStart,') or s.startswith('I-Start,'):
        if tag in s.replace(' ', ''):
            p = [x.strip() for x in s.split(',')]
            if p[8].strip('"').lower().endswith('hoi4.exe'):
                base = int(p[3], 16)
    elif s.startswith('CSwitch,'):
        if tag not in s.replace(' ', ''):
            continue
        p = [x.strip() for x in s.split(',')]
        if p[1] == 'TimeStamp':
            continue
        if p[8].replace(' ', '').endswith(tag):
            cswitch_wait[int(p[9])][p[13]] += 1
flush()

excl_func = collections.defaultdict(collections.Counter)
for cls, c in raw_excl.items():
    for ip, k in c.items():
        f = rva_to_func(ip - base)
        excl_func[cls][f if f is not None else -1] += k
incl_func = collections.defaultdict(collections.Counter)
edges = collections.Counter()
for cls, lst in raw_stacks.items():
    for st in lst:
        fs = [f for f in (rva_to_func(ip - base) for ip in st) if f is not None]
        for f in set(fs):
            incl_func[cls][f] += 1
        if cls == 'main':
            for a, b in set(zip(fs[1:], fs[:-1])):
                edges[(a, b)] += 1

res = {
    'runtime_base': hex(base) if base else None, 'lines': n,
    'duration_s': (tmax - tmin) / 1e6 if tmin is not None else 0,
    'per_cpu': {c: dict(v) for c, v in sorted(per_cpu.items())},
    'thread_samples': thread_samples.most_common(),
    'thread_cpu': {t: sorted(c.items()) for t, c in thread_cpu.items()},
    'excl_mod': {k: v.most_common(40) for k, v in excl_mod.items()},
    'excl_func': {k: [(hex(f) if f > 0 else str(f), c) for f, c in v.most_common(300)] for k, v in excl_func.items()},
    'incl_func': {k: [(hex(f), c) for f, c in v.most_common(400)] for k, v in incl_func.items()},
    'stack_count': dict(stack_count),
    'main_edges': [(hex(a), hex(b), c) for (a, b), c in edges.most_common(5000)],
    'cswitch_wait': {t: c.most_common(10) for t, c in cswitch_wait.items()},
}
json.dump(res, open(OUT, 'w'), indent=1)
print('done', n, 'lines; base', res['runtime_base'], 'duration', res['duration_s'])
