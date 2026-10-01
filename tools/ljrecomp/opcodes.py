"""NMOS 6502/6510 opcode table (documented and undocumented opcodes).

This table is the single source of truth for instruction decoding. It is
used to generate the C instruction macros (runtime/cpu_ops_gen.h) and by
the recompiler for decoding and code generation.

Modes:
  imp  implied            acc  accumulator         imm  #$nn
  zp   $nn                zpx  $nn,X               zpy  $nn,Y
  abs  $nnnn              abx  $nnnn,X             aby  $nnnn,Y
  izx  ($nn,X)            izy  ($nn),Y             ind  ($nnnn)
  rel  branch offset

Kinds:
  R  read memory operand     W  write memory operand
  M  read-modify-write       I  implied / register / stack
  F  control flow            J  CPU jam (KIL)
"""

MODE_LEN = {
    "imp": 1, "acc": 1, "imm": 2, "zp": 2, "zpx": 2, "zpy": 2,
    "abs": 3, "abx": 3, "aby": 3, "izx": 2, "izy": 2, "ind": 3, "rel": 2,
}

# (mnemonic, mode) for every opcode 0x00..0xFF
_ROWS = """
BRK imp|ORA izx|JAM imp|SLO izx|NOP zp |ORA zp |ASL zp |SLO zp |PHP imp|ORA imm|ASL acc|ANC imm|NOP abs|ORA abs|ASL abs|SLO abs
BPL rel|ORA izy|JAM imp|SLO izy|NOP zpx|ORA zpx|ASL zpx|SLO zpx|CLC imp|ORA aby|NOP imp|SLO aby|NOP abx|ORA abx|ASL abx|SLO abx
JSR abs|AND izx|JAM imp|RLA izx|BIT zp |AND zp |ROL zp |RLA zp |PLP imp|AND imm|ROL acc|ANC imm|BIT abs|AND abs|ROL abs|RLA abs
BMI rel|AND izy|JAM imp|RLA izy|NOP zpx|AND zpx|ROL zpx|RLA zpx|SEC imp|AND aby|NOP imp|RLA aby|NOP abx|AND abx|ROL abx|RLA abx
RTI imp|EOR izx|JAM imp|SRE izx|NOP zp |EOR zp |LSR zp |SRE zp |PHA imp|EOR imm|LSR acc|ALR imm|JMP abs|EOR abs|LSR abs|SRE abs
BVC rel|EOR izy|JAM imp|SRE izy|NOP zpx|EOR zpx|LSR zpx|SRE zpx|CLI imp|EOR aby|NOP imp|SRE aby|NOP abx|EOR abx|LSR abx|SRE abx
RTS imp|ADC izx|JAM imp|RRA izx|NOP zp |ADC zp |ROR zp |RRA zp |PLA imp|ADC imm|ROR acc|ARR imm|JMP ind|ADC abs|ROR abs|RRA abs
BVS rel|ADC izy|JAM imp|RRA izy|NOP zpx|ADC zpx|ROR zpx|RRA zpx|SEI imp|ADC aby|NOP imp|RRA aby|NOP abx|ADC abx|ROR abx|RRA abx
NOP imm|STA izx|NOP imm|SAX izx|STY zp |STA zp |STX zp |SAX zp |DEY imp|NOP imm|TXA imp|XAA imm|STY abs|STA abs|STX abs|SAX abs
BCC rel|STA izy|JAM imp|SHA izy|STY zpx|STA zpx|STX zpy|SAX zpy|TYA imp|STA aby|TXS imp|TAS aby|SHY abx|STA abx|SHX aby|SHA aby
LDY imm|LDA izx|LDX imm|LAX izx|LDY zp |LDA zp |LDX zp |LAX zp |TAY imp|LDA imm|TAX imp|LXA imm|LDY abs|LDA abs|LDX abs|LAX abs
BCS rel|LDA izy|JAM imp|LAX izy|LDY zpx|LDA zpx|LDX zpy|LAX zpy|CLV imp|LDA aby|TSX imp|LAS aby|LDY abx|LDA abx|LDX aby|LAX aby
CPY imm|CMP izx|NOP imm|DCP izx|CPY zp |CMP zp |DEC zp |DCP zp |INY imp|CMP imm|DEX imp|SBX imm|CPY abs|CMP abs|DEC abs|DCP abs
BNE rel|CMP izy|JAM imp|DCP izy|NOP zpx|CMP zpx|DEC zpx|DCP zpx|CLD imp|CMP aby|NOP imp|DCP aby|NOP abx|CMP abx|DEC abx|DCP abx
CPX imm|SBC izx|NOP imm|ISC izx|CPX zp |SBC zp |INC zp |ISC zp |INX imp|SBC imm|NOP imp|SBC imm|CPX abs|SBC abs|INC abs|ISC abs
BEQ rel|SBC izy|JAM imp|ISC izy|NOP zpx|SBC zpx|INC zpx|ISC zpx|SED imp|SBC aby|NOP imp|ISC aby|NOP abx|SBC abx|INC abx|ISC abx
"""

READ_OPS = {"LDA", "LDX", "LDY", "EOR", "AND", "ORA", "ADC", "SBC", "CMP",
            "CPX", "CPY", "BIT", "LAX", "NOP", "LAS", "ANC", "ALR", "ARR",
            "SBX", "XAA", "LXA"}
WRITE_OPS = {"STA", "STX", "STY", "SAX", "SHA", "SHX", "SHY", "TAS"}
RMW_OPS = {"ASL", "LSR", "ROL", "ROR", "INC", "DEC", "SLO", "RLA", "SRE",
           "RRA", "DCP", "ISC"}
FLOW_OPS = {"BRK", "JSR", "RTI", "RTS", "JMP", "BPL", "BMI", "BVC", "BVS",
            "BCC", "BCS", "BNE", "BEQ"}
BRANCH_OPS = {"BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ"}

# Undocumented opcodes. The recompiler accepts the stable ones and leaves
# the unstable ones to the interpreter.
UNDOC = {"SLO", "RLA", "SRE", "RRA", "SAX", "LAX", "DCP", "ISC", "ANC",
         "ALR", "ARR", "SBX", "LAS", "SHA", "SHX", "SHY", "TAS", "XAA",
         "LXA", "JAM"}
UNSTABLE = {"SHA", "SHX", "SHY", "TAS", "XAA", "LXA", "JAM"}


def _build():
    table = []
    rows = [r for r in _ROWS.strip().splitlines()]
    assert len(rows) == 16
    for r in rows:
        cells = r.split("|")
        assert len(cells) == 16, r
        for c in cells:
            mn, mode = c.split()
            table.append((mn, mode))
    out = []
    for op, (mn, mode) in enumerate(table):
        if mn == "JAM":
            kind = "J"
        elif mn in FLOW_OPS:
            kind = "F"
        elif mode in ("imp",):
            kind = "I"
        elif mode == "acc":
            kind = "M"
        elif mn in RMW_OPS:
            kind = "M"
        elif mn in WRITE_OPS:
            kind = "W"
        elif mn in READ_OPS:
            kind = "R"
        else:
            raise ValueError((hex(op), mn, mode))
        # undocumented NOP variants (all NOPs except $EA) are undocumented
        undoc = mn in UNDOC or (mn == "NOP" and op != 0xEA) or (mn == "SBC" and op == 0xEB)
        out.append({"op": op, "mn": mn, "mode": mode, "kind": kind,
                    "len": MODE_LEN[mode], "undoc": undoc,
                    "unstable": mn in UNSTABLE})
    return out


OPCODES = _build()


def base_cycles(o):
    """Cycles without page crossing / branch penalties."""
    mn, mode, kind = o["mn"], o["mode"], o["kind"]
    if kind == "J":
        return 0
    if kind == "F":
        return {"BRK": 7, "JSR": 6, "RTI": 6, "RTS": 6}.get(mn) or (
            3 if (mn == "JMP" and mode == "abs") else 5 if mn == "JMP" else 2)
    if kind == "I":
        return {"PHA": 3, "PHP": 3, "PLA": 4, "PLP": 4}.get(mn, 2)
    if kind == "R":
        return {"imm": 2, "zp": 3, "zpx": 4, "zpy": 4, "abs": 4, "abx": 4,
                "aby": 4, "izx": 6, "izy": 5}[mode]
    if kind == "W":
        return {"zp": 3, "zpx": 4, "zpy": 4, "abs": 4, "abx": 5, "aby": 5,
                "izx": 6, "izy": 6}[mode]
    if kind == "M":
        return {"acc": 2, "zp": 5, "zpx": 6, "abs": 6, "abx": 7, "aby": 7,
                "izx": 8, "izy": 8}[mode]
    raise ValueError(o)


def has_page_penalty(o):
    return o["kind"] == "R" and o["mode"] in ("abx", "aby", "izy")


if __name__ == "__main__":
    for o in OPCODES:
        print("%02X %s %s %s %d %d" % (o["op"], o["mn"], o["mode"], o["kind"],
                                       o["len"], base_cycles(o)))
