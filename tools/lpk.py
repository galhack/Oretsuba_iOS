#!/usr/bin/env python3
"""Lucifen LPK archive reader (list / extract).

Format notes follow GARbro's ArcLPK.cs (MIT). Usage:
  lpk.py list  ARCHIVE.LPK [--keys k1:k2]
  lpk.py extract ARCHIVE.LPK OUTDIR [--keys k1:k2] [--match SUBSTR]
"""
import os
import struct
import sys

BASE_KEY1 = 0xA5B9AC6B
BASE_KEY2 = 0x9A639DE5
CONTENT_XOR = 0x5D
ROTATE_PATTERN = 0x31746285
M32 = 0xFFFFFFFF
WHOLE_TABLE = bytes((((v ^ CONTENT_XOR) >> 4) | ((v ^ CONTENT_XOR) << 4)) & 0xFF for v in range(256))


def rotl(v, n):
    n &= 31
    return ((v << n) | (v >> (32 - n))) & M32 if n else v


def rotr(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & M32 if n else v


def decrypt_words(data, length, key, index_mode):
    words = list(struct.unpack_from('<%dI' % (length // 4), data, 0))
    pattern = ROTATE_PATTERN
    for i in range(len(words)):
        words[i] ^= key
        if index_mode:
            pattern = rotl(pattern, 4)
            key = rotr(key, pattern)
        else:
            pattern = rotr(pattern, 4)
            key = rotl(key, pattern)
    struct.pack_into('<%dI' % len(words), data, 0, *words)


def lzss(src, out_size):
    out = bytearray()
    win = bytearray(4096)
    wp = 0xFEE
    i = 0
    n = len(src)
    while len(out) < out_size and i < n:
        ctl = src[i]
        i += 1
        for bit in range(8):
            if len(out) >= out_size or i >= n:
                break
            if ctl & (1 << bit):
                b = src[i]
                i += 1
                out.append(b)
                win[wp] = b
                wp = (wp + 1) & 0xFFF
            else:
                if i + 1 >= n:
                    i = n
                    break
                lo, hi = src[i], src[i + 1]
                i += 2
                off = lo | ((hi & 0xF0) << 4)
                cnt = (hi & 0x0F) + 3
                for k in range(cnt):
                    b = win[(off + k) & 0xFFF]
                    out.append(b)
                    win[wp] = b
                    wp = (wp + 1) & 0xFFF
    return bytes(out[:out_size])


class Lpk:
    def __init__(self, path, file_key=None):
        self.path = path
        self.f = open(path, 'rb')
        name = os.path.splitext(os.path.basename(path))[0].upper().encode('cp932')
        k1, k2 = BASE_KEY1, BASE_KEY2
        b, e = 0, len(name) - 1
        while e >= 0:
            k1 ^= name[e]
            k2 ^= name[b]
            k1 = rotr(k1, 7)
            k2 = rotl(k2, 7)
            b += 1
            e -= 1
        if file_key:
            k1 ^= file_key[0]
            k2 ^= file_key[1]
        self.key1 = k1
        hdr = self.f.read(8)
        if hdr[:4] != b'LPK1':
            raise ValueError('not an LPK1 archive')
        code = struct.unpack_from('<I', hdr, 4)[0] ^ k2
        size = code & 0xFFFFFF
        flags = code >> 24
        if flags & 1:
            size = (size << 11) - 8
        self.flags = flags
        self.aligned = bool(flags & 1)
        self.encrypted = bool(flags & 4)
        self.packed = bool(flags & 8)
        self.whole = bool(flags & 0x10)
        if size < 5 or size > os.path.getsize(path):
            raise ValueError('bad index size %x (wrong key?)' % size)
        index = bytearray(self.f.read(size))
        decrypt_words(index, size, k2, True)
        self.index = bytes(index)
        self.entries = []
        self._parse()
        if self.whole and name == b'PATCH':
            self.whole = False

    def _parse(self):
        ix = self.index
        count = struct.unpack_from('<I', ix, 0)[0]
        p = 4
        plen = ix[p]
        p += 1
        self.prefix = ix[p:p + plen] if plen else b''
        p += plen
        self.width = 4 if ix[p] else 2
        p += 1
        letters = struct.unpack_from('<I', ix, p)[0]
        p += 4
        self.entries_off = p + letters
        esize = 13 if self.packed else 9
        if (len(ix) - self.entries_off) // esize < count:
            esize = (len(ix) - self.entries_off) // count
        self.esize = esize
        self._walk(p, b'')
        if len(self.entries) != count:
            raise ValueError('entry count mismatch %d != %d' % (len(self.entries), count))

    def _walk(self, p, name):
        ix = self.index
        n = ix[p]
        p += 1
        for _ in range(n):
            letter = ix[p]
            p += 1
            if self.width == 4:
                nxt = struct.unpack_from('<i', ix, p)[0]
            else:
                nxt = struct.unpack_from('<H', ix, p)[0]
            p += self.width
            if letter:
                self._walk(p + nxt, name + bytes([letter]))
            else:
                self._add(name, nxt)

    def _add(self, name, num):
        pos = self.entries_off + self.esize * num
        flag = 0
        if self.esize & 1:
            flag = self.index[pos]
            pos += 1
        off, size = struct.unpack_from('<II', self.index, pos)
        if self.aligned:
            off <<= 11
        unpacked = 0
        if self.packed:
            unpacked = struct.unpack_from('<I', self.index, pos + 8)[0]
        self.entries.append({'name': name.decode('cp932'), 'offset': off, 'size': size,
                             'unpacked': unpacked, 'flag': flag})

    def read(self, ent):
        self.f.seek(ent['offset'])
        # recorded size includes the common prefix stripped from each entry
        data = self.f.read(max(ent['size'] - len(self.prefix), 0) if self.prefix else ent['size'])
        if ent['unpacked']:
            data = lzss(data, ent['unpacked'])
        data = bytearray(data)
        if self.whole:
            data = bytearray(bytes(data).translate(WHOLE_TABLE))
        if self.encrypted:
            n = min(len(data), 0x100) & ~3
            if n:
                decrypt_words(data, n, self.key1, False)
        return self.prefix + bytes(data)


def parse_keys(argv):
    if '--keys' in argv:
        k = argv[argv.index('--keys') + 1].split(':')
        return int(k[0], 16), int(k[1], 16)
    return None


def main(argv):
    cmd, path = argv[1], argv[2]
    lpk = Lpk(path, parse_keys(argv))
    if cmd == 'list':
        print('flags=%02x entries=%d prefix=%r' % (lpk.flags, len(lpk.entries), lpk.prefix))
        for e in lpk.entries:
            print('%-24s off=%10d size=%9d unpacked=%9d flag=%d' % (e['name'], e['offset'], e['size'], e['unpacked'], e['flag']))
    elif cmd == 'extract':
        out = argv[3]
        match = argv[argv.index('--match') + 1] if '--match' in argv else None
        os.makedirs(out, exist_ok=True)
        for e in lpk.entries:
            if match and match not in e['name']:
                continue
            with open(os.path.join(out, e['name']), 'wb') as f:
                f.write(lpk.read(e))


if __name__ == '__main__':
    main(sys.argv)
