"""Fail if the XBE has 0F 31 byte pairs anywhere except the expected RDTSC code sites.
Cerbios (AdvCPUSupport) patches 0F 31 pairs at load on overclocked consoles."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
allowed_exec = int(sys.argv[2]) if len(sys.argv) > 2 else 8
base = struct.unpack_from('<I', d, 0x104)[0]
nsec, sa = struct.unpack_from('<II', d, 0x11C)
bad = 0
for i in range(nsec):
    fl, va, vs, ra, rs, na = struct.unpack_from('<IIIIII', d, sa - base + i * 56)
    sec = d[ra:ra + rs]
    hits = [va + j for j in range(len(sec) - 1) if sec[j] == 0x0F and sec[j + 1] == 0x31]
    if fl & 4:
        print('code section %d: %d pairs' % (i, len(hits)))
        if len(hits) > allowed_exec: bad = 1
    elif hits:
        print('DATA section %d has pairs at %s' % (i, [hex(h) for h in hits])); bad = 1
print('FAIL' if bad else 'OK')
sys.exit(bad)
