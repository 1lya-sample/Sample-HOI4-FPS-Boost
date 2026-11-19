"""Check that each patch signature matches exactly once in hoi4.exe and where.
usage: python sigcheck.py <hoi4.exe>"""
import sys, struct, re

SIGS = {
    'wake-all (fn start)': '48 89 5C 24 10 56 48 83 EC 20 48 63 41 18 33 DB 48 8B F1 90 85 D2 75 51 44 8B C3 8B D3 85 C0 7E 25 4C 8B 0E 48 8B C8 49 8D 81 98 00 00 00 66 90 80 38 01 74 17 41 FF C0 48 FF C2 48 05 A0 00 00 00 48 3B D1 7C EA 8B 46 18 90 EB 22 49 63 C0 48 8D 0C 80 48 C1 E1 05 49 03 C9 48 8B 5C 24 38 48 83 C4 20 5E E9 ?? ?? ?? ?? 83 FA 01 75 2F 85 C0 7E 2B',
    'province-throttle': '83 BE 64 17 00 00 00 7E 04 B3 01 EB 27 84 DB 75 23 B8 1F 85 EB 51 41 F7 ED C1 FA 05 8B C2 C1 E8 1F 03 D0 6B C2 64 41 8B CD 2B C8 41 3B 8F 30 01 00 00 75 ??',
    'worker-spin call': '49 63 46 38 48 8D 0C 80 48 C1 E1 05 48 03 0E E8 ?? ?? ?? ?? 90 48 8D 4C 24 50 E8',
    'worker sleep fn': '40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 85 C0 75 ?? 8B 43 4C 3D FF FF FF 7F 74 ?? 80 BB 98 00 00 00 02',
    'frame-pacing code': 'FF 90 F0 02 00 00 F3 0F 10 53 1C 8D 4F 01 F3 0F 10 0D ?? ?? ?? ?? 33 D2 84 C0 0F 44 D1 48 8D 0D ?? ?? ?? ?? 48 63 C2 F3 0F 10 34 81',
}

d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]; nsec = struct.unpack_from('<H', d, pe + 6)[0]; osz = struct.unpack_from('<H', d, pe + 20)[0]
so = pe + 24 + osz
for _ in range(nsec):
    nm = d[so:so + 8].rstrip(b'\0').decode(); vs, va, rs, rp = struct.unpack_from('<IIII', d, so + 8); so += 40
    if nm == '.text':
        tva, trp, trs = va, rp, rs
text = d[trp:trp + trs]
for name, sig in SIGS.items():
    rx = re.compile(b''.join(b'.' if t == '??' else re.escape(bytes([int(t, 16)])) for t in sig.split()), re.S)
    hits = [tva + m.start() for m in rx.finditer(text)]
    print(f'{name:22} {len(hits)} match(es): {[hex(h) for h in hits[:5]]}')
