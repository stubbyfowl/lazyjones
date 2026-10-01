"""Build the code map: which addresses hold game instructions.

Input: execution traces from host/ljexplore and the game image. Output: a
list of instruction start addresses with flags. The code map holds no game
bytes, only addresses, so it can be kept in the repository.

Flags:
  DYN     operand bytes change at run time (self modifying code); the
          generated code reads the operand from memory when it runs
  STATIC  found by static control flow analysis, not seen in a trace
"""
import json

from .opcodes import OPCODES
from .trace import TR_EXEC, TR_ROM, TR_VAR_OP, TR_VAR_OP1, TR_VAR_OP2

DYN = 1
STATIC = 2

CODE_LO = 0x0200      # below: zero page and stack, never compiled
CODE_HI = 0xA000      # $A000 and up can be ROM: never compiled


def compilable(op):
    o = OPCODES[op]
    return o['kind'] != 'J' and not o['unstable']


def in_range(a, n):
    return CODE_LO <= a and a + n <= CODE_HI


def from_traces(traces):
    """Instructions seen executing in RAM, with their DYN flags."""
    code = {}
    t0 = traces[0]
    for t in traces[1:]:
        t0.merge(t)
    t = t0
    for a in range(CODE_LO, CODE_HI):
        f = t.flags[a]
        if not (f & TR_EXEC) or (f & TR_ROM):
            continue
        if f & TR_VAR_OP:
            continue  # the opcode itself changes: leave to the interpreter
        op = t.first[a][0]
        o = OPCODES[op]
        if not compilable(op) or not in_range(a, o['len']):
            continue
        flags = 0
        if f & (TR_VAR_OP1 | TR_VAR_OP2):
            flags |= DYN
        for i in range(1, o['len']):
            if t.smc[a + i]:
                flags |= DYN
        code[a] = flags
    return code, t


def successors(a, mem):
    o = OPCODES[mem[a]]
    mn, mode, n = o['mn'], o['mode'], o['len']
    nxt = a + n
    if mn in ('RTS', 'RTI', 'BRK') or o['kind'] == 'J':
        return []
    if mn == 'JMP':
        if mode == 'abs':
            return [mem[a + 1] | (mem[a + 2] << 8)]
        return []
    if mn == 'JSR':
        return [mem[a + 1] | (mem[a + 2] << 8), nxt]
    if mode == 'rel':
        off = mem[a + 1]
        if off & 0x80:
            off -= 256
        return [nxt + off, nxt]
    return [nxt]


def extend_static(code, mem, trace):
    """Add instructions reachable from known code by static analysis.

    Conservative: a new instruction is accepted only when it decodes to a
    documented opcode, lies in RAM, does not overlap known instructions and
    none of its bytes were ever written by the game (written bytes are data
    or modified code)."""
    owner = {}
    for a in code:
        for i in range(OPCODES[mem[a]]['len']):
            owner.setdefault(a + i, a)
    added = 0
    work = list(code)
    while work:
        a = work.pop()
        for s in successors(a, mem):
            if s in code or not (CODE_LO <= s < CODE_HI):
                continue
            op = mem[s]
            o = OPCODES[op]
            if o['undoc'] or not compilable(op) or not in_range(s, o['len']):
                continue
            if any((s + i) in owner for i in range(o['len'])):
                continue
            if any(trace.writes[s + i] for i in range(o['len'])):
                continue
            code[s] = STATIC
            for i in range(o['len']):
                owner[s + i] = s
            work.append(s)
            added += 1
    return added


def save(path, code, info):
    out = {
        'game': 'Lazy Jones (C64, 1984)',
        'note': 'Instruction start addresses of the game code (no game data). '
                'flags: 1 = operand modified at run time, 2 = found by static analysis.',
        'entry': '%04X' % info.get('entry', 0x080D),
        'instructions': [['%04X' % a, code[a]] for a in sorted(code)],
    }
    with open(path, 'w') as f:
        json.dump(out, f, indent=0)
        f.write('\n')


def load(path):
    with open(path) as f:
        d = json.load(f)
    return {int(a, 16): fl for a, fl in d['instructions']}
