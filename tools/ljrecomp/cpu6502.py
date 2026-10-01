"""Small 6502 interpreter used to run depackers at build time.

Some cracked versions of the game are compressed. Running their own
depacker code on a flat 64 KiB memory gives the plain game image. Only
documented opcodes are needed; decimal mode is supported for completeness.
"""

from .opcodes import OPCODES


class CPU:
    def __init__(self, mem):
        self.m = mem
        self.a = self.x = self.y = 0
        self.sp = 0xFF
        self.pc = 0
        self.n = self.v = self.d = self.i = self.z = self.c = 0
        self.steps = 0

    # -- helpers --
    def rd(self, a):
        return self.m[a & 0xFFFF]

    def wr(self, a, v):
        self.m[a & 0xFFFF] = v & 0xFF

    def fetch(self):
        v = self.m[self.pc]
        self.pc = (self.pc + 1) & 0xFFFF
        return v

    def fetch16(self):
        lo = self.fetch()
        return lo | (self.fetch() << 8)

    def push(self, v):
        self.m[0x100 | self.sp] = v & 0xFF
        self.sp = (self.sp - 1) & 0xFF

    def pull(self):
        self.sp = (self.sp + 1) & 0xFF
        return self.m[0x100 | self.sp]

    def nz(self, v):
        v &= 0xFF
        self.n = v >> 7
        self.z = int(v == 0)
        return v

    def get_p(self):
        return (self.n << 7) | (self.v << 6) | 0x30 | (self.d << 3) | (self.i << 2) | (self.z << 1) | self.c

    def set_p(self, p):
        self.n = (p >> 7) & 1
        self.v = (p >> 6) & 1
        self.d = (p >> 3) & 1
        self.i = (p >> 2) & 1
        self.z = (p >> 1) & 1
        self.c = p & 1

    # -- addressing --
    def ea(self, mode):
        if mode == 'zp':
            return self.fetch()
        if mode == 'zpx':
            return (self.fetch() + self.x) & 0xFF
        if mode == 'zpy':
            return (self.fetch() + self.y) & 0xFF
        if mode == 'abs':
            return self.fetch16()
        if mode == 'abx':
            return (self.fetch16() + self.x) & 0xFFFF
        if mode == 'aby':
            return (self.fetch16() + self.y) & 0xFFFF
        if mode == 'izx':
            p = (self.fetch() + self.x) & 0xFF
            return self.m[p] | (self.m[(p + 1) & 0xFF] << 8)
        if mode == 'izy':
            p = self.fetch()
            return ((self.m[p] | (self.m[(p + 1) & 0xFF] << 8)) + self.y) & 0xFFFF
        raise ValueError(mode)

    def adc(self, m):
        if not self.d:
            s = self.a + m + self.c
            self.v = int((~(self.a ^ m) & (self.a ^ s) & 0x80) != 0)
            self.c = int(s > 0xFF)
            self.a = self.nz(s)
        else:
            lo = (self.a & 0x0F) + (m & 0x0F) + self.c
            if lo >= 0x0A:
                lo = ((lo + 6) & 0x0F) + 0x10
            s = (self.a & 0xF0) + (m & 0xF0) + lo
            self.z = int(((self.a + m + self.c) & 0xFF) == 0)
            self.n = (s >> 7) & 1
            self.v = int((~(self.a ^ m) & (self.a ^ s) & 0x80) != 0)
            if s >= 0xA0:
                s += 0x60
            self.c = int(s >= 0x100)
            self.a = s & 0xFF

    def sbc(self, m):
        b = self.a - m - (1 - self.c)
        self.v = int(((self.a ^ m) & (self.a ^ b) & 0x80) != 0)
        if not self.d:
            self.c = int(b >= 0)
            self.a = self.nz(b)
            return
        lo = (self.a & 0x0F) - (m & 0x0F) + self.c - 1
        if lo < 0:
            lo = ((lo - 6) & 0x0F) - 0x10
        r = (self.a & 0xF0) - (m & 0xF0) + lo
        if r < 0:
            r -= 0x60
        self.c = int(b >= 0)
        self.nz(b)
        self.a = r & 0xFF

    def cmp(self, r, m):
        self.c = int(r >= m)
        self.nz(r - m)

    def branch(self, cond):
        off = self.fetch()
        if cond:
            self.pc = (self.pc + (off - 256 if off & 0x80 else off)) & 0xFFFF

    def step(self):
        op = self.fetch()
        o = OPCODES[op]
        mn, mode = o['mn'], o['mode']
        self.steps += 1
        if o['undoc']:
            raise RuntimeError('undocumented opcode $%02X at $%04X' % (op, (self.pc - 1) & 0xFFFF))
        if mn in ('LDA', 'LDX', 'LDY', 'EOR', 'AND', 'ORA', 'ADC', 'SBC', 'CMP', 'CPX', 'CPY', 'BIT'):
            m = self.fetch() if mode == 'imm' else self.rd(self.ea(mode))
            if mn == 'LDA': self.a = self.nz(m)
            elif mn == 'LDX': self.x = self.nz(m)
            elif mn == 'LDY': self.y = self.nz(m)
            elif mn == 'EOR': self.a = self.nz(self.a ^ m)
            elif mn == 'AND': self.a = self.nz(self.a & m)
            elif mn == 'ORA': self.a = self.nz(self.a | m)
            elif mn == 'ADC': self.adc(m)
            elif mn == 'SBC': self.sbc(m)
            elif mn == 'CMP': self.cmp(self.a, m)
            elif mn == 'CPX': self.cmp(self.x, m)
            elif mn == 'CPY': self.cmp(self.y, m)
            elif mn == 'BIT':
                self.n = (m >> 7) & 1
                self.v = (m >> 6) & 1
                self.z = int((self.a & m) == 0)
            return
        if mn in ('STA', 'STX', 'STY'):
            a = self.ea(mode)
            self.wr(a, {'STA': self.a, 'STX': self.x, 'STY': self.y}[mn])
            return
        if mn in ('ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC'):
            if mode == 'acc':
                v = self.a
            else:
                a = self.ea(mode)
                v = self.rd(a)
            if mn == 'ASL':
                self.c = v >> 7; v = (v << 1) & 0xFF
            elif mn == 'LSR':
                self.c = v & 1; v >>= 1
            elif mn == 'ROL':
                c = self.c; self.c = v >> 7; v = ((v << 1) | c) & 0xFF
            elif mn == 'ROR':
                c = self.c; self.c = v & 1; v = (v >> 1) | (c << 7)
            elif mn == 'INC':
                v = (v + 1) & 0xFF
            else:
                v = (v - 1) & 0xFF
            self.nz(v)
            if mode == 'acc':
                self.a = v
            else:
                self.wr(a, v)
            return
        if mn == 'JMP':
            t = self.fetch16()
            if mode == 'ind':
                t = self.m[t] | (self.m[(t & 0xFF00) | ((t + 1) & 0xFF)] << 8)
            self.pc = t
            return
        if mn == 'JSR':
            t = self.fetch16()
            ret = (self.pc - 1) & 0xFFFF
            self.push(ret >> 8)
            self.push(ret)
            self.pc = t
            return
        if mn == 'RTS':
            lo = self.pull()
            self.pc = ((lo | (self.pull() << 8)) + 1) & 0xFFFF
            return
        if mn == 'RTI':
            self.set_p(self.pull())
            lo = self.pull()
            self.pc = lo | (self.pull() << 8)
            return
        if mn == 'BRK':
            raise RuntimeError('BRK at $%04X' % ((self.pc - 1) & 0xFFFF))
        br = {'BPL': lambda: not self.n, 'BMI': lambda: self.n, 'BVC': lambda: not self.v,
              'BVS': lambda: self.v, 'BCC': lambda: not self.c, 'BCS': lambda: self.c,
              'BNE': lambda: not self.z, 'BEQ': lambda: self.z}
        if mn in br:
            self.branch(br[mn]())
            return
        if mn == 'NOP': return
        if mn == 'CLC': self.c = 0; return
        if mn == 'SEC': self.c = 1; return
        if mn == 'CLI': self.i = 0; return
        if mn == 'SEI': self.i = 1; return
        if mn == 'CLV': self.v = 0; return
        if mn == 'CLD': self.d = 0; return
        if mn == 'SED': self.d = 1; return
        if mn == 'TAX': self.x = self.nz(self.a); return
        if mn == 'TAY': self.y = self.nz(self.a); return
        if mn == 'TXA': self.a = self.nz(self.x); return
        if mn == 'TYA': self.a = self.nz(self.y); return
        if mn == 'TSX': self.x = self.nz(self.sp); return
        if mn == 'TXS': self.sp = self.x; return
        if mn == 'INX': self.x = self.nz(self.x + 1); return
        if mn == 'INY': self.y = self.nz(self.y + 1); return
        if mn == 'DEX': self.x = self.nz(self.x - 1); return
        if mn == 'DEY': self.y = self.nz(self.y - 1); return
        if mn == 'PHA': self.push(self.a); return
        if mn == 'PHP': self.push(self.get_p()); return
        if mn == 'PLA': self.a = self.nz(self.pull()); return
        if mn == 'PLP': self.set_p(self.pull()); return
        raise RuntimeError('unhandled %s' % mn)
