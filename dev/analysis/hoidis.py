"""Disassemble functions of hoi4.exe by RVA, with call targets, imports and strings annotated.
usage: python -I dis.py <exe> <names.json or -> <rva> [<rva> ...]
"""
import sys, struct, json, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

exe, names_path, rvas = sys.argv[1], sys.argv[2], [int(x, 16) for x in sys.argv[3:]]
names = json.load(open(names_path)) if names_path != '-' else {}
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; osz = struct.unpack_from('<H', d, pe + 20)[0]
opt = pe + 24
IMAGE_BASE = struct.unpack_from('<Q', d, opt + 24)[0]
irva = struct.unpack_from('<I', d, opt + 112 + 8)[0]
exc_rva, exc_size = struct.unpack_from('<II', d, opt + 112 + 24)
secs = []; so = opt + osz
for _ in range(nsec):
    nm = d[so:so + 8].rstrip(b'\0').decode('latin1')
    vs, va, rs, rp = struct.unpack_from('<IIII', d, so + 8); secs.append((nm, va, vs, rs, rp)); so += 40


def off(r):
    for nm, va, vs, rs, rp in secs:
        if va <= r < va + min(vs, rs):
            return r - va + rp
    return None


def sec_of(r):
    for nm, va, vs, rs, rp in secs:
        if va <= r < va + max(vs, rs):
            return nm
    return '?'


cstr = lambda o: d[o:d.index(b'\0', o)].decode('latin1')
iat = {}
o = off(irva)
while True:
    olt, ts, fc, name, ft = struct.unpack_from('<IIIII', d, o)
    if name == 0:
        break
    dll = cstr(off(name)); o += 20
    t = off(olt or ft); slot = ft
    while True:
        v = struct.unpack_from('<Q', d, t)[0]
        if v == 0:
            break
        iat[slot] = dll + '!' + (cstr(off(v & 0x7fffffff) + 2) if not v >> 63 else '#%d' % (v & 0xffff))
        t += 8; slot += 8

chunks = {}
po = off(exc_rva)
for i in range(exc_size // 12):
    b, e, u = struct.unpack_from('<III', d, po + i * 12)
    if not b:
        continue
    root, uu = b, u
    for _ in range(32):
        if uu & 1:
            break
        uo = off(uu)
        if uo is None or not (d[uo] >> 3) & 4:
            break
        cnt = d[uo + 2]
        cb, ce, cu = struct.unpack_from('<III', d, uo + 4 + 2 * ((cnt + 1) & ~1)); root, uu = cb, cu
    chunks.setdefault(root, []).append((b, e))

str_re = re.compile(rb'[\x20-\x7e]{4,100}\x00')


def describe(target):
    if target in iat:
        return iat[target]
    if hex(target) in names:
        s = [x for x in names[hex(target)]['strings'] if 'gamestate unitil' not in x and 'ThreadForbid' not in x][:2]
        return 'fn_%x %s' % (target, ' | '.join(x[:40] for x in s))
    if target in chunks:
        return 'fn_%x' % target
    o = off(target)
    if o is not None:
        m = str_re.match(d, o)
        if m:
            return repr(m.group(0)[:-1].decode('latin1')[:60])
    return '%s:%x' % (sec_of(target), target)


md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False
for r in rvas:
    root = r
    if r not in chunks:
        # find containing function
        for k, ch in chunks.items():
            if any(b <= r < e for b, e in ch):
                root = k
                break
    print('=' * 20, 'fn_%x' % root, 'chunks', [(hex(b), hex(e)) for b, e in sorted(chunks.get(root, []))])
    for b, e in sorted(chunks.get(root, [(r, r + 0x200)])):
        code = d[off(b):off(b) + (e - b)]
        for ins in md.disasm(code, b):
            txt = f'{ins.address:8x}: {ins.mnemonic:6} {ins.op_str}'
            m = re.search(r'\[rip ([+-]) 0x([0-9a-f]+)\]', ins.op_str)
            note = ''
            if m:
                disp = int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
                note = describe(ins.address + ins.size + disp)
            elif ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
                t = int(ins.op_str, 16)
                if not any(bb <= t < ee for bb, ee in chunks.get(root, [])):
                    note = describe(t)
            print(txt + (('   ; ' + note) if note else ''))
