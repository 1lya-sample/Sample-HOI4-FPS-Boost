"""Reconstruct the main thread's frame / tick timeline from sampled stacks (xperf dump on stdin).
usage: xperf ... | python -I phases.py <pid> <main_tid> <exe> <out.json>

Every 1 ms sample of the main thread is classified by the exe functions on its stack:
  R = frame work (render / map / UI), H = hourly tick, D = daily tick (inside hourly),
  S = other simulation work, O = other, W = not running (no sample).
Runs of R separated by simulation are counted as frames; daily-tick runs count days.
"""
import sys, json, bisect, struct, collections

pid, mtid, exe, out = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
sys.path.insert(0, '')
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
    if not b:
        continue
    root, uu = b, u
    for _ in range(32):
        if uu & 1:
            break
        uo = off(uu)
        if not (d[uo] >> 3) & 4:
            break
        cnt = d[uo + 2]
        cb, ce, cu = struct.unpack_from('<III', d, uo + 4 + 2 * ((cnt + 1) & ~1)); root, uu = cb, cu
    funcs[b] = (e, root)
starts = sorted(funcs)


def fn(rva):
    i = bisect.bisect_right(starts, rva) - 1
    if i >= 0:
        e, root = funcs[starts[i]]
        if rva < e:
            return root
    return None


RENDER = {0x2b8f00, 0xbab870, 0xb90300, 0x2b6480, 0x22bb820, 0xe8b990, 0x2bb5a0}
SIM = 0x1b3b50
HOURLY = 0x1f34d0
DAILY = 0x1eae60

tag = '(%s)' % pid
base = None
samples = []     # (ts, None) for each main-thread sample; class filled from stack
idx = {}
cur = None
for line in sys.stdin:
    s = line.lstrip()
    if s.startswith('SampledProfile,'):
        p = [x.strip() for x in s.split(',')]
        if p[1] == 'TimeStamp' or p[3] != mtid or not p[2].replace(' ', '').endswith(tag):
            continue
        idx[p[1]] = len(samples)
        samples.append([int(p[1]), set()])
    elif s.startswith('Stack,'):
        p = [x.strip() for x in s.split(',')]
        if p[1] == 'TimeStamp' or p[2] != mtid or p[1] not in idx:
            continue
        if p[5].lower().startswith('hoi4.exe'):
            samples[idx[p[1]]][1].add(int(p[4], 16))
    elif base is None and s.startswith('I-DCStart,') and tag in s.replace(' ', ''):
        p = [x.strip() for x in s.split(',')]
        if p[8].strip('"').lower().endswith('hoi4.exe'):
            base = int(p[3], 16)

seq = []
for ts, ips in samples:
    fs = {fn(ip - base) for ip in ips}
    if DAILY in fs:
        c = 'D'
    elif HOURLY in fs:
        c = 'H'
    elif SIM in fs:
        c = 'S'
    elif fs & RENDER:
        c = 'R'
    else:
        c = 'O'
    seq.append((ts, c))
seq.sort()

# frames: maximal runs where class R appears, allowing small gaps of O/W (<= 2 ms)
frames = []      # (start_ts, end_ts)
days = []
cur_f = None
last_d = None
for ts, c in seq:
    if c == 'R':
        if cur_f and ts - cur_f[1] <= 3000:
            cur_f[1] = ts
        else:
            if cur_f:
                frames.append(tuple(cur_f))
            cur_f = [ts, ts]
    if c == 'D':
        if last_d is None or ts - last_d > 3000:
            days.append(ts)
        last_d = ts
if cur_f:
    frames.append(tuple(cur_f))
t0, t1 = seq[0][0], seq[-1][0]
dur = (t1 - t0) / 1e6
gaps = [(frames[i + 1][0] - frames[i][0]) / 1000 for i in range(len(frames) - 1)]
gaps.sort()
pct = lambda q: gaps[int(q * (len(gaps) - 1))] if gaps else 0
render_ms = sorted((e - s) / 1000 + 1 for s, e in frames)
res = {
    'duration_s': dur, 'main_samples': len(seq),
    'frames': len(frames), 'fps': len(frames) / dur,
    'frame_interval_ms_p10_p50_p90': [pct(0.1), pct(0.5), pct(0.9)],
    'render_ms_per_frame_p50': render_ms[len(render_ms) // 2] if render_ms else 0,
    'daily_ticks': len(days), 'days_per_s': len(days) / dur,
    'class_share': {k: sum(1 for _, c in seq if c == k) / len(seq) for k in 'RHDSO'},
}
# what fills the long gaps between frames (> 50 ms)
lg = collections.Counter(); nlong = 0
for i in range(len(frames) - 1):
    a, b = frames[i][1], frames[i + 1][0]
    if (b - a) / 1000 > 50:
        nlong += 1
        for ts, c in seq:
            if a < ts < b: lg[c] += 1
res['long_gaps'] = {'count': nlong, 'ms_by_class': dict(lg)}
# timeline sample: first 400 ms as a string
res['timeline_first_400ms'] = ''.join(c for _, c in seq[:400])
json.dump(res, open(out, 'w'), indent=1)
print(json.dumps({k: v for k, v in res.items() if k != 'timeline_first_400ms'}, indent=1))
print(res['timeline_first_400ms'])
