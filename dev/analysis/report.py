"""Print a comparison report for one or more profiles made by agg.py.
usage: python -I report.py <names.json> <prof.json> [<prof.json> ...]
"""
import sys, json, collections

names = json.load(open(sys.argv[1]))
NOISE = ('gamestate unitil', 'ThreadForbid', 'NeededSize', 'Instance not created')


def lab(f, k=3):
    d = names.get(f)
    if not d:
        return ''
    s = [x for x in d['strings'] if len(x) > 6 and not any(z in x for z in NOISE)][:k]
    return ' | '.join(x[:55] for x in s)


for path in sys.argv[2:]:
    p = json.load(open(path))
    dur = p['duration_s']
    print('=' * 100)
    print(path, f'duration {dur:.1f}s')
    mt = max(p['thread_samples'], key=lambda x: x[1])
    main = next(c for t, c in p['thread_samples'] if t == 28664) if any(t == 28664 for t, c in p['thread_samples']) else 0
    workers = sum(c for t, c in p['thread_samples'] if t != 28664)
    print(f'main thread busy {main/1000:.1f}s = {100*main/1000/dur:.0f}% of wall; workers {workers/1000:.1f} cpu-s = {workers/1000/dur:.2f} cores')
    c6 = p['per_cpu'].get('6', {})
    print('cpu6:', c6, ' cpu7:', p['per_cpu'].get('7', {}))
    for cls in ('main',):
        m = p['excl_mod'].get(cls, [])
        s = sum(c for _, c in m) or 1
        print('main modules:', [(k, round(100 * c / s, 1)) for k, c in m[:8]])
    print('main waits:', p['cswitch_wait'].get('28664'))
    st = p['stack_count'].get('main', 1)
    incl = dict(p['incl_func'].get('main', []))
    ch = collections.defaultdict(list)
    for a, b, c in p['main_edges']:
        ch[a].append((c, b))

    def tree(f, depth, maxd, thr):
        for c, b in sorted(ch.get(f, []), reverse=True):
            pct = 100 * c / st
            if pct < thr:
                continue
            print('  ' * depth + f'{pct:5.1f}% {b} [{names.get(b, {}).get("size", "?")}B] {lab(b)}')
            if depth < maxd:
                tree(b, depth + 1, maxd, thr)

    print('--- main loop children (0xe41670):')
    tree('0xe41670', 1, 4, 1.0)
    print('--- main exclusive top:')
    for f, c in p['excl_func'].get('main', [])[:20]:
        print(f'  {100*c/max(main,1):5.1f}% {f} [{names.get(f, {}).get("size", "?")}B] {lab(f)}')
