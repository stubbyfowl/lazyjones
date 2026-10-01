"""Generate C code from the game image and the code map.

Every instruction becomes a labelled block of C that expands the shared
instruction macro (runtime/cpu_ops_gen.h) with constant operands, followed
by an event check. Control flow becomes goto inside a 2 KiB chunk, or a
return to the dispatcher. The generated code keeps exact cycle counts, so
it behaves like the interpreter cycle for cycle.

Safety: each block first checks C.instr_mod[addr]. When the game has
changed the bytes of that instruction, the block returns 0 and the
interpreter runs the instruction instead.
"""
from .opcodes import OPCODES
from .analyze import DYN

CHUNK_SHIFT = 11
NCHUNKS = 0x10000 >> CHUNK_SHIFT

BRANCH_COND = {
    'BPL': '!(C.cpu.fn & 0x80)', 'BMI': '(C.cpu.fn & 0x80)',
    'BVC': '!C.cpu.fv', 'BVS': 'C.cpu.fv',
    'BCC': '!C.cpu.fc', 'BCS': 'C.cpu.fc',
    'BNE': 'C.cpu.fz', 'BEQ': '!C.cpu.fz',
}


def disasm(mem, a):
    o = OPCODES[mem[a]]
    mn, mode = o['mn'], o['mode']
    b1 = mem[a + 1] if o['len'] > 1 else 0
    w = b1 | (mem[a + 2] << 8) if o['len'] > 2 else 0
    if mode == 'imp':
        return mn
    if mode == 'acc':
        return mn + ' A'
    if mode == 'imm':
        return '%s #$%02X' % (mn, b1)
    if mode == 'zp':
        return '%s $%02X' % (mn, b1)
    if mode == 'zpx':
        return '%s $%02X,X' % (mn, b1)
    if mode == 'zpy':
        return '%s $%02X,Y' % (mn, b1)
    if mode == 'abs':
        return '%s $%04X' % (mn, w)
    if mode == 'abx':
        return '%s $%04X,X' % (mn, w)
    if mode == 'aby':
        return '%s $%04X,Y' % (mn, w)
    if mode == 'izx':
        return '%s ($%02X,X)' % (mn, b1)
    if mode == 'izy':
        return '%s ($%02X),Y' % (mn, b1)
    if mode == 'ind':
        return '%s ($%04X)' % (mn, w)
    if mode == 'rel':
        off = b1 - 256 if b1 & 0x80 else b1
        return '%s $%04X' % (mn, (a + 2 + off) & 0xFFFF)
    return mn


class Gen:
    def __init__(self, mem, code):
        self.mem = mem          # 64 KiB bytearray with the game image
        self.code = code        # {addr: flags}
        self.out = []

    def chunk_of(self, a):
        return a >> CHUNK_SHIFT

    def emit(self, s):
        self.out.append(s)

    def transfer(self, chunk, target, ind='    '):
        """Code to continue at a constant target address."""
        t = target & 0xFFFF
        self.emit(ind + 'if (LJ_UNLIKELY(C.clk >= C.ev)) { C.cpu.pc = 0x%04X; return 1; }' % t)
        if t in self.code and self.chunk_of(t) == chunk:
            self.emit(ind + 'goto L_%04X;' % t)
        else:
            self.emit(ind + 'C.cpu.pc = 0x%04X; return 1;' % t)

    def instr(self, a, chunk, next_emitted):
        mem = self.mem
        op = mem[a]
        o = OPCODES[op]
        n = o['len']
        nxt = (a + n) & 0xFFFF
        dyn = bool(self.code[a] & DYN)
        mn, mode, kind = o['mn'], o['mode'], o['kind']
        self.emit('L_%04X: /* %04X: %s%s */' % (a, a, disasm(mem, a), ' (operand modified at run time)' if dyn else ''))
        self.emit('    if (LJ_UNLIKELY(C.instr_mod[0x%04X])) { C.cpu.pc = 0x%04X; return 0; }' % (a, a))
        if dyn:
            op1 = 'C.ram[0x%04X]' % (a + 1)
            op16 = '(uint16_t)(C.ram[0x%04X] | (C.ram[0x%04X] << 8))' % (a + 1, a + 2)
        else:
            op1 = '0x%02X' % mem[a + 1] if n > 1 else ''
            op16 = '0x%04X' % (mem[a + 1] | (mem[a + 2] << 8)) if n > 2 else ''
        if kind in ('R', 'W', 'M', 'I'):
            arg = '' if n == 1 else (op1 if n == 2 else op16)
            self.emit('    OPC_%02X(%s);' % (op, arg))
            self.fallthrough(nxt, chunk, next_emitted)
            return
        if mn == 'JMP' and mode == 'abs':
            self.emit('    C.clk += 3;')
            if dyn:
                self.emit('    C.cpu.pc = %s;' % op16)
                self.redispatch()
            else:
                self.transfer(chunk, mem[a + 1] | (mem[a + 2] << 8))
            return
        if mn == 'JMP':
            self.emit('    { uint16_t p_ = %s; uint8_t lo_ = RD(p_, 3);' % op16)
            self.emit('      uint8_t hi_ = RD((uint16_t)((p_ & 0xFF00) | ((p_ + 1) & 0x00FF)), 4);')
            self.emit('      C.cpu.pc = (uint16_t)(lo_ | (hi_ << 8)); C.clk += 5; }')
            self.redispatch()
            return
        if mn == 'JSR':
            ret = (a + 2) & 0xFFFF
            if dyn:
                self.emit('    { uint16_t t_ = %s; cpu_push(0x%02X); cpu_push(0x%02X); C.clk += 6; C.cpu.pc = t_; }'
                          % (op16, ret >> 8, ret & 0xFF))
                self.redispatch()
            else:
                self.emit('    cpu_push(0x%02X); cpu_push(0x%02X); C.clk += 6;' % (ret >> 8, ret & 0xFF))
                self.transfer(chunk, mem[a + 1] | (mem[a + 2] << 8))
            return
        if mn == 'RTS':
            self.emit('    { uint8_t lo_ = cpu_pull(); uint8_t hi_ = cpu_pull();')
            self.emit('      C.cpu.pc = (uint16_t)((lo_ | (hi_ << 8)) + 1); C.clk += 6; }')
            self.redispatch()
            return
        if mn == 'RTI':
            self.emit('    { cpu_set_p(cpu_pull()); uint8_t lo_ = cpu_pull(); uint8_t hi_ = cpu_pull();')
            self.emit('      C.cpu.pc = (uint16_t)(lo_ | (hi_ << 8)); C.clk += 6; cpu_irq_recheck_now(); }')
            self.redispatch()
            return
        if mn == 'BRK':
            ret = (a + 2) & 0xFFFF
            self.emit('    cpu_push(0x%02X); cpu_push(0x%02X); cpu_push((uint8_t)(cpu_get_p() | 0x30));'
                      % (ret >> 8, ret & 0xFF))
            self.emit('    C.cpu.fi = 1;')
            self.emit('    { uint8_t lo_ = RD(0xFFFE, 5); uint8_t hi_ = RD(0xFFFF, 6);')
            self.emit('      C.cpu.pc = (uint16_t)(lo_ | (hi_ << 8)); C.clk += 7; }')
            self.redispatch()
            return
        if mn in BRANCH_COND:
            cond = BRANCH_COND[mn]
            if dyn:
                self.emit('    if (%s) {' % cond)
                self.emit('        uint16_t t_ = (uint16_t)(0x%04X + (int8_t)C.ram[0x%04X]);' % (nxt, a + 1))
                self.emit('        C.clk += 3 + (((unsigned)(0x%04X ^ t_) >> 8) & 1u);' % nxt)
                self.emit('        C.cpu.pc = t_;')
                self.emit('        if (LJ_UNLIKELY(C.clk >= C.ev)) return 1;')
                self.emit('        goto redispatch;')
                self.emit('    }')
            else:
                off = mem[a + 1]
                if off & 0x80:
                    off -= 256
                t = (nxt + off) & 0xFFFF
                pen = 1 if (t ^ nxt) & 0xFF00 else 0
                self.emit('    if (%s) {' % cond)
                self.emit('        C.clk += %d;' % (3 + pen))
                self.transfer(chunk, t, '        ')
                self.emit('    }')
            self.emit('    C.clk += 2;')
            self.fallthrough(nxt, chunk, next_emitted)
            return
        raise ValueError('cannot compile %s at $%04X' % (mn, a))

    def fallthrough(self, nxt, chunk, next_emitted):
        self.emit('    if (LJ_UNLIKELY(C.clk >= C.ev)) { C.cpu.pc = 0x%04X; return 1; }' % nxt)
        if next_emitted == nxt:
            return
        if nxt in self.code and self.chunk_of(nxt) == chunk:
            self.emit('    goto L_%04X;' % nxt)
        else:
            self.emit('    C.cpu.pc = 0x%04X; return 1;' % nxt)

    def redispatch(self):
        self.emit('    if (LJ_UNLIKELY(C.clk >= C.ev)) return 1;')
        self.emit('    goto redispatch;')

    def chunk(self, cid, addrs):
        self.emit('')
        self.emit('/* $%04X-$%04X */' % (cid << CHUNK_SHIFT, ((cid + 1) << CHUNK_SHIFT) - 1))
        self.emit('static int chunk_%02X(void)' % cid)
        self.emit('{')
        self.emit('    goto dispatch;')
        self.emit('redispatch:')
        self.emit('    if ((C.cpu.pc >> %d) != 0x%02X) return 1;' % (CHUNK_SHIFT, cid))
        self.emit('dispatch:')
        self.emit('    switch (C.cpu.pc) {')
        for a in addrs:
            self.emit('    case 0x%04X: goto L_%04X;' % (a, a))
        self.emit('    default: return 0;')
        self.emit('    }')
        for i, a in enumerate(addrs):
            nxt_emitted = addrs[i + 1] if i + 1 < len(addrs) else None
            self.instr(a, cid, nxt_emitted)
        self.emit('}')

    def generate(self, header):
        self.emit(header)
        self.emit('#include "cpu_ops.h"')
        self.emit('')
        self.emit('#if defined(__clang__)')
        self.emit('#pragma clang diagnostic ignored "-Wunused-label"')
        self.emit('#elif defined(__GNUC__)')
        self.emit('#pragma GCC diagnostic ignored "-Wunused-label"')
        self.emit('#endif')
        chunks = {}
        for a in sorted(self.code):
            chunks.setdefault(self.chunk_of(a), []).append(a)
        for cid in sorted(chunks):
            self.chunk(cid, chunks[cid])
        self.emit('')
        self.emit('typedef int (*chunk_fn)(void);')
        self.emit('static const chunk_fn chunks[%d] = {' % NCHUNKS)
        for cid in range(NCHUNKS):
            self.emit('    %s,' % ('chunk_%02X' % cid if cid in chunks else '0'))
        self.emit('};')
        self.emit('')
        self.emit('const int recomp_available = 1;')
        self.emit('')
        self.emit('/* Run recompiled code from C.cpu.pc. Returns 0 when the instruction')
        self.emit(' * at C.cpu.pc must be interpreted, 1 when the caller must handle an')
        self.emit(' * event first. */')
        self.emit('int recomp_run(void)')
        self.emit('{')
        self.emit('    for (;;) {')
        self.emit('        chunk_fn f = chunks[C.cpu.pc >> %d];' % CHUNK_SHIFT)
        self.emit('        if (!f || !f())')
        self.emit('            return 0;')
        self.emit('        if (C.clk >= C.ev)')
        self.emit('            return 1;')
        self.emit('    }')
        self.emit('}')
        return '\n'.join(self.out) + '\n'


def generate_recomp(mem, code, header):
    return Gen(mem, code).generate(header)


def generate_data(image, info, code, header):
    """C file with the game image and the list of compiled instructions."""
    load = info['load']
    out = [header, '#include <stdint.h>', '']
    out.append('const uint16_t lj_game_load = 0x%04X;' % load)
    out.append('const uint16_t lj_game_entry = 0x%04X;' % info['entry'])
    out.append('const char lj_game_release[] = "%s";' % info['release'].replace('"', "'"))
    out.append('const char lj_game_hash[] = "%s";' % info['hash'])
    out.append('const uint32_t lj_game_image_len = %d;' % len(image))
    out.append('const uint8_t lj_game_image[%d] = {' % len(image))
    for i in range(0, len(image), 24):
        out.append('    ' + ', '.join('0x%02X' % b for b in image[i:i + 24]) + ',')
    out.append('};')
    out.append('')
    addrs = sorted(code)
    out.append('const uint32_t lj_code_count = %d;' % len(addrs))
    out.append('/* address, length, operand modified at run time */')
    out.append('const uint16_t lj_code_addr[%d] = {' % len(addrs))
    for i in range(0, len(addrs), 12):
        out.append('    ' + ', '.join('0x%04X' % a for a in addrs[i:i + 12]) + ',')
    out.append('};')
    out.append('const uint8_t lj_code_info[%d] = {' % len(addrs))
    vals = []
    for a in addrs:
        n = OPCODES[image[a - load]]['len']
        vals.append(n | (0x80 if code[a] & DYN else 0))
    for i in range(0, len(vals), 24):
        out.append('    ' + ', '.join('0x%02X' % v for v in vals[i:i + 24]) + ',')
    out.append('};')
    return '\n'.join(out) + '\n'
