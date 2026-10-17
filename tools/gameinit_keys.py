#!/usr/bin/env python3
"""Extract per-archive LPK keys from gameinit.sob (GARbro ParseGameInit heuristic)."""
import struct, sys

def parse(sob):
    assert sob[:4] == b'SOB0'
    off = struct.unpack_from('<I', sob, 4)[0] + 8
    ilen = struct.unpack_from('<I', sob, off)[0]
    base = off + 8
    n = (len(sob) - base) // 4
    w = struct.unpack_from('<%dI' % n, sob, base)
    keys = {}
    i = 0
    last = ilen // 4 - 28
    while i < last:
        p = w[i:i + 29]
        if (p[0] == 0x28 and p[1] == 0 and p[2] != 0 and p[3] == 8 and p[4] == 1 and p[5] == 8 and p[6] == 1
                and p[7] == 8 and p[8] != 0 and p[9] == 8 and p[10] != 0 and p[11] == 5 and p[17] == 0x28
                and p[18] == 0 and p[19] != 0 and p[20] == 8 and p[21] != 0 and p[22] == 8 and p[23] == 0xffffffff
                and p[24] == 8 and p[25] == 1 and p[26] == 8 and p[27] == 1 and p[28] == 5):
            name_at = base + i * 4 + p[21] - p[2] + 0x34
            name = sob[name_at:sob.index(b'\0', name_at)].decode('cp932').upper()
            keys[name] = (p[8], p[10])
            i += 0x22
        else:
            i += 1
    return keys

if __name__ == '__main__':
    for k, v in parse(open(sys.argv[1], 'rb').read()).items():
        print('%s %08x:%08x' % (k, v[0], v[1]))
