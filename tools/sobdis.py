#!/usr/bin/env python3
"""Disassembler for Lucifen SOB0/SOB2 threaded bytecode.

Operands are 32-bit words. Relocated words are shown symbolically:
  G[n]   address of user variable n (user_var.sob)
  N_xxx  native function index
  T[n]   entry n of the third table
  L+off  address inside FUNC_LIB
  @off   address inside this blob
Usage: sobdis.py FILE.sob [--strings]
"""
import struct
import sys

from sob import Sob

# opcode -> (mnemonic, operand count); -1 marks the switch table
OPS = {
    0: ('end', 0), 1: ('ret', 0), 2: ('retv', 1), 3: ('getret', 1), 4: ('call', 1),
    5: ('native', 3), 6: ('jmp', 1), 7: ('switch', -1), 8: ('push', 1), 9: ('pushm', 1),
    10: ('pop', 1), 11: ('spadd', 1), 12: ('spsub', 1), 13: ('add', 3), 14: ('sub', 3),
    15: ('mul', 3), 16: ('div', 3), 17: ('rem', 3), 18: ('mod', 3), 19: ('and', 3),
    20: ('or', 3), 21: ('xor', 3), 22: ('not', 2), 23: ('lnot', 2), 24: ('sar', 3),
    25: ('shl', 3), 26: ('incm', 1), 27: ('decm', 1), 28: ('incs', 1), 29: ('decs', 1),
    30: ('jz', 2), 31: ('movi', 2), 32: ('movm', 2), 33: ('movmi', 2), 34: ('movsi', 2),
    35: ('movsm', 2), 36: ('movms', 2), 37: ('movss', 2), 38: ('movsd', 2), 39: ('movmd', 2),
    40: ('lea', 2), 41: ('lt', 3), 42: ('le', 3), 43: ('gt', 3), 44: ('ge', 3),
    45: ('eq', 3), 46: ('ne', 3),
}


class Dis:
    def __init__(self, sob):
        self.s = sob
        self.rel = {}
        kinds = ['G', 'N', 'T', 'L', '@']
        for k, table in enumerate(sob.relocs[:5]):
            for off, val in table:
                self.rel[off] = (kinds[k], val)
        self.targets = set()
        for off, val in sob.relocs[4]:
            self.targets.add(val)

    def opnd(self, off):
        w = self.s.word(off)
        r = self.rel.get(off)
        if r is None:
            return '%d' % w if w < 0x10000 else '0x%x' % w
        k, v = r
        if k == 'G':
            return 'G[%d]' % v
        if k == 'N':
            return 'N_%x' % v
        if k == 'T':
            return 'T[%d]' % v
        if k == 'L':
            return 'L+%x' % v
        return '@%x' % v

    def string_at(self, off):
        b = self.s.blob
        if off >= len(b):
            return None
        pre = ''
        if b[off] == 0 and off + 3 < len(b):
            pre = '{%02x%02x}' % (b[off + 1], b[off + 2])
            off += 3
        end = b.find(b'\0', off)
        try:
            return pre + b[off:end].decode('cp932')
        except UnicodeDecodeError:
            return None

    def run(self, out):
        s = self.s
        pc = 0
        while pc < s.code_size:
            op = s.word(pc)
            lab = '%05x' % pc
            if pc in self.targets:
                out.write('L_%x:\n' % pc)
            if op not in OPS:
                out.write('%s  .word 0x%x\n' % (lab, op))
                pc += 4
                continue
            name, n = OPS[op]
            if n == -1:
                idx = self.opnd(pc + 4)
                ents = []
                p = pc + 8
                while (p in self.rel and self.rel[p][0] == '@') and p < s.code_size:
                    ents.append('@%x' % self.rel[p][1])
                    p += 4
                out.write('%s  switch %s [%s]\n' % (lab, idx, ', '.join(ents)))
                pc = p
                continue
            args = [self.opnd(pc + 4 + 4 * i) for i in range(n)]
            note = ''
            for i in range(n):
                r = self.rel.get(pc + 4 + 4 * i)
                if r and r[0] == '@' and r[1] >= s.code_size:
                    st = self.string_at(r[1])
                    if st is not None:
                        note = '  ; "%s"' % st.replace('\n', '\\n')[:80]
            out.write('%s  %-7s %s%s\n' % (lab, name, ', '.join(args), note))
            pc += 4 + 4 * n


def main():
    sob = Sob(open(sys.argv[1], 'rb').read())
    Dis(sob).run(sys.stdout)


if __name__ == '__main__':
    main()
