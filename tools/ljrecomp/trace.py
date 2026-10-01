"""Reader for the binary trace written by host/ljexplore (runtime/trace.h)."""
import struct

TR_EXEC = 0x01
TR_VAR_OP = 0x02
TR_VAR_OP1 = 0x04
TR_VAR_OP2 = 0x08
TR_IND_TARGET = 0x10
TR_ROM = 0x20


class Trace:
    def __init__(self, data):
        o = 0
        self.flags = data[o:o + 65536]; o += 65536
        first = data[o:o + 3 * 65536]; o += 3 * 65536
        self.first = [first[3 * i:3 * i + 3] for i in range(65536)]
        self.writes = struct.unpack_from('<65536I', data, o); o += 4 * 65536
        self.in_code = data[o:o + 65536]; o += 65536
        self.smc = data[o:o + 65536]; o += 65536
        self.known = data[o:o + 65536]; o += 65536
        self.uninit_reads, = struct.unpack_from('<I', data, o); o += 4
        self.uninit_addr = struct.unpack_from('<256H', data, o); o += 512
        self.uninit_pc = struct.unpack_from('<256H', data, o); o += 512
        o = (o + 7) & ~7
        self.instructions, = struct.unpack_from('<Q', data, o)

    @classmethod
    def load(cls, path):
        with open(path, 'rb') as f:
            return cls(f.read())

    def merge(self, other):
        """Combine two traces (union of coverage and observations)."""
        flags = bytearray(self.flags)
        for a in range(65536):
            fa, fb = self.flags[a], other.flags[a]
            if (fa & TR_EXEC) and (fb & TR_EXEC) and self.first[a] != other.first[a]:
                op_a, op_b = self.first[a], other.first[a]
                if op_a[0] != op_b[0]:
                    fa |= TR_VAR_OP
                if op_a[1] != op_b[1]:
                    fa |= TR_VAR_OP1
                if op_a[2] != op_b[2]:
                    fa |= TR_VAR_OP2
            if not (fa & TR_EXEC) and (fb & TR_EXEC):
                self.first[a] = other.first[a]
            flags[a] = fa | fb
        self.flags = bytes(flags)
        self.smc = bytes(x | y for x, y in zip(self.smc, other.smc))
        self.in_code = bytes(x | y for x, y in zip(self.in_code, other.in_code))
        self.writes = tuple(min(0xFFFFFFFF, x + y) for x, y in zip(self.writes, other.writes))
        self.instructions += other.instructions
