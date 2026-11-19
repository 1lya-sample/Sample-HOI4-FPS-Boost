"""Label functions of a PE by the strings they reference (RIP-relative LEA/MOV scan).

usage: python -I names.py <exe> <out.json> <prof.json> [<prof.json> ...]
Labels the top functions of every given profile.
"""
import sys, struct, json, re

exe, out, profs = sys.argv[1], sys.argv[2], sys.argv[3:]
data = open(exe, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsz = struct.unpack_from('<H', data, pe + 20)[0]
opt = pe + 24
exc_rva, exc_size = struct.unpack_from('<II', data, opt + 112 + 3 * 8)
secs = []
so = opt + optsz
for _ in range(nsec):
    vsize, va, rsize, rptr = struct.unpack_from('<IIII', data, so + 8)
    secs.append((va, min(vsize, rsize) if rsize else vsize, rptr))
    so += 40


def off(rva):
    for va, sz, rp in secs:
        if va <= rva < va + sz:
            return rva - va + rp
    return None


chunks = {}
po = off(exc_rva)
for i in range(exc_size // 12):
    b, e, u = struct.unpack_from('<III', data, po + i * 12)
    if not b:
        continue
    root, uu = b, u
    for _ in range(32):
        if uu & 1:
            break
        uo = off(uu)
        if uo is None or not (data[uo] >> 3) & 4:
            break
        cnt = data[uo + 2]
        cb, ce, cu = struct.unpack_from('<III', data, uo + 4 + 2 * ((cnt + 1) & ~1))
        root, uu = cb, cu
    chunks.setdefault(root, []).append((b, e))

str_re = re.compile(rb'[\x20-\x7e]{5,120}\x00')


def string_at(rva):
    o = off(rva)
    if o is None:
        return None
    m = str_re.match(data, o)
    return m.group(0)[:-1].decode('latin1') if m else None


def refs(root):
    res = []
    for b, e in chunks.get(root, []):
        o = off(b)
        if o is None:
            continue
        code = data[o:o + (e - b)]
        for i in range(len(code) - 7):
            if code[i] in (0x48, 0x4C) and code[i + 1] in (0x8D, 0x8B) and (code[i + 2] & 0xC7) == 0x05:
                tgt = b + i + 7 + struct.unpack_from('<i', code, i + 3)[0]
                s = string_at(tgt)
                if s and s not in res:
                    res.append(s)
    return res


want = set()
for path in profs:
    prof = json.load(open(path))
    for k in ('excl_func', 'incl_func'):
        for cls, lst in prof[k].items():
            for f, c in lst[:200]:
                if f.startswith('0x'):
                    want.add(int(f, 16))
    for a, b, c in prof.get('main_edges', [])[:1500]:
        want.add(int(a, 16)); want.add(int(b, 16))
outd = {}
for f in want:
    outd[hex(f)] = {'strings': refs(f)[:25], 'size': sum(e - b for b, e in chunks.get(f, []))}
json.dump(outd, open(out, 'w'), indent=1)
print('labelled', len(outd))
