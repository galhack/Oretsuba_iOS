#!/usr/bin/env python3
"""SOB (Lucifen script) container parser."""
import struct

class Sob:
    def __init__(self, data):
        self.data = data
        self.magic = data[:4]
        hsize = struct.unpack_from('<I', data, 4)[0]
        p = 8
        end = 8 + hsize
        self.relocs = []
        while p < end:
            n = struct.unpack_from('<I', data, p)[0]
            p += 4
            self.relocs.append([struct.unpack_from('<II', data, p + 8 * i) for i in range(n)])
            p += 8 * n
        self.code_size, self.blob_size = struct.unpack_from('<II', data, end)
        self.blob = data[end + 8:end + 8 + self.blob_size]

    def word(self, off):
        return struct.unpack_from('<I', self.blob, off)[0]

def load(path):
    return Sob(open(path, 'rb').read())
