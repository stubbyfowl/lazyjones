#!/usr/bin/env python3
"""Generate runtime/cpu_ops_gen.h from tools/ljrecomp/opcodes.py.

Every non control-flow opcode becomes one macro OPC_xx(operand). The macro
performs the complete instruction, including bus accesses at their exact
cycle offsets and the cycle count, but does not touch the program counter.
The interpreter (runtime/cpu_interp.c) and the recompiled code
(tools/ljrecomp/codegen.py) both expand these macros, so both execute the
same semantics.

Usage: python3 tools/gen_cpu_ops.py > runtime/cpu_ops_gen.h
       python3 tools/gen_cpu_ops.py --interp > runtime/cpu_interp_gen.inc
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from ljrecomp.opcodes import OPCODES, base_cycles  # noqa: E402

READ_BODY = {
    "LDA": "C.cpu.a = m_; SETNZ(m_);",
    "LDX": "C.cpu.x = m_; SETNZ(m_);",
    "LDY": "C.cpu.y = m_; SETNZ(m_);",
    "LAX": "C.cpu.a = C.cpu.x = m_; SETNZ(m_);",
    "EOR": "op_eor(m_);",
    "AND": "op_and(m_);",
    "ORA": "op_ora(m_);",
    "ADC": "op_adc(m_);",
    "SBC": "op_sbc(m_);",
    "CMP": "op_cmp(C.cpu.a, m_);",
    "CPX": "op_cmp(C.cpu.x, m_);",
    "CPY": "op_cmp(C.cpu.y, m_);",
    "BIT": "op_bit(m_);",
    "NOP": "(void)m_;",
    "LAS": "{ uint8_t t_ = (uint8_t)(m_ & C.cpu.sp); C.cpu.a = C.cpu.x = C.cpu.sp = t_; SETNZ(t_); }",
    "ANC": "op_and(m_); C.cpu.fc = C.cpu.a >> 7;",
    "ALR": "C.cpu.a &= m_; C.cpu.a = op_lsr(C.cpu.a);",
    "ARR": "op_arr(m_);",
    "SBX": "{ unsigned t_ = (unsigned)(C.cpu.a & C.cpu.x) - m_; C.cpu.fc = t_ < 0x100u; C.cpu.x = (uint8_t)t_; SETNZ(C.cpu.x); }",
    "XAA": "C.cpu.a = (uint8_t)((C.cpu.a | 0xEE) & C.cpu.x & m_); SETNZ(C.cpu.a);",
    "LXA": "C.cpu.a = C.cpu.x = (uint8_t)((C.cpu.a | 0xEE) & m_); SETNZ(C.cpu.a);",
}

RMW_FN = {
    "ASL": "op_asl", "LSR": "op_lsr", "ROL": "op_rol", "ROR": "op_ror",
    "INC": "op_inc", "DEC": "op_dec", "SLO": "op_slo", "RLA": "op_rla",
    "SRE": "op_sre", "RRA": "op_rra", "DCP": "op_dcp", "ISC": "op_isc",
}

IMPLIED_BODY = {
    "NOP": "",
    "CLC": "C.cpu.fc = 0;",
    "SEC": "C.cpu.fc = 1;",
    "CLV": "C.cpu.fv = 0;",
    "CLD": "C.cpu.fd = 0;",
    "SED": "C.cpu.fd = 1;",
    "SEI": "C.cpu.fi = 1;",
    "TAX": "C.cpu.x = C.cpu.a; SETNZ(C.cpu.x);",
    "TAY": "C.cpu.y = C.cpu.a; SETNZ(C.cpu.y);",
    "TXA": "C.cpu.a = C.cpu.x; SETNZ(C.cpu.a);",
    "TYA": "C.cpu.a = C.cpu.y; SETNZ(C.cpu.a);",
    "TSX": "C.cpu.x = C.cpu.sp; SETNZ(C.cpu.x);",
    "TXS": "C.cpu.sp = C.cpu.x;",
    "INX": "C.cpu.x++; SETNZ(C.cpu.x);",
    "INY": "C.cpu.y++; SETNZ(C.cpu.y);",
    "DEX": "C.cpu.x--; SETNZ(C.cpu.x);",
    "DEY": "C.cpu.y--; SETNZ(C.cpu.y);",
    "PHA": "cpu_push(C.cpu.a);",
    "PHP": "cpu_push((uint8_t)(cpu_get_p() | 0x30));",
    "PLA": "C.cpu.a = cpu_pull(); SETNZ(C.cpu.a);",
}

EA = {
    "zp": "uint8_t ea_ = (uint8_t)(o_);",
    "zpx": "uint8_t ea_ = (uint8_t)((o_) + C.cpu.x);",
    "zpy": "uint8_t ea_ = (uint8_t)((o_) + C.cpu.y);",
    "abs": "uint16_t ea_ = (uint16_t)(o_);",
    "abx": "uint16_t b_ = (uint16_t)(o_); uint16_t ea_ = (uint16_t)(b_ + C.cpu.x); unsigned px_ = ((unsigned)(b_ ^ ea_) >> 8) & 1u;",
    "aby": "uint16_t b_ = (uint16_t)(o_); uint16_t ea_ = (uint16_t)(b_ + C.cpu.y); unsigned px_ = ((unsigned)(b_ ^ ea_) >> 8) & 1u;",
    "izx": "uint8_t p_ = (uint8_t)((o_) + C.cpu.x); uint16_t ea_ = (uint16_t)(ZRD(p_) | (ZRD((uint8_t)(p_ + 1)) << 8));",
    "izy": "uint8_t p_ = (uint8_t)(o_); uint16_t b_ = (uint16_t)(ZRD(p_) | (ZRD((uint8_t)(p_ + 1)) << 8)); uint16_t ea_ = (uint16_t)(b_ + C.cpu.y); unsigned px_ = ((unsigned)(b_ ^ ea_) >> 8) & 1u;",
}

WRONG = "(uint16_t)((b_ & 0xFF00) | (ea_ & 0x00FF))"


def write_value(mn):
    return {
        "STA": "C.cpu.a", "STX": "C.cpu.x", "STY": "C.cpu.y",
        "SAX": "(uint8_t)(C.cpu.a & C.cpu.x)",
    }.get(mn)


def gen(o):
    mn, mode, kind = o["mn"], o["mode"], o["kind"]
    cyc = base_cycles(o)
    if kind == "I":
        if mn == "CLI":
            body = "C.cpu.fi = 0; C.clk += 2; cpu_irq_recheck_delayed();"
            return f"#define OPC_{o['op']:02X}() do {{ {body} }} while (0)"
        if mn == "PLP":
            body = "cpu_set_p(cpu_pull()); C.clk += 4; cpu_irq_recheck_delayed();"
            return f"#define OPC_{o['op']:02X}() do {{ {body} }} while (0)"
        body = IMPLIED_BODY[mn]
        return f"#define OPC_{o['op']:02X}() do {{ {body} C.clk += {cyc}; }} while (0)"
    if kind == "R":
        body = READ_BODY[mn]
        if mode == "imm":
            s = f"uint8_t m_ = (uint8_t)(o_); {body} C.clk += 2;"
        elif mode in ("zp", "zpx", "zpy"):
            s = f"{EA[mode]} uint8_t m_ = ZRD(ea_); {body} C.clk += {cyc};"
        elif mode == "abs":
            s = f"{EA[mode]} uint8_t m_ = RD(ea_, 3); {body} C.clk += 4;"
        elif mode in ("abx", "aby"):
            s = (f"{EA[mode]} if (px_) DRD({WRONG}, 3); "
                 f"uint8_t m_ = RD(ea_, 3 + px_); {body} C.clk += 4 + px_;")
        elif mode == "izx":
            s = f"{EA[mode]} uint8_t m_ = RD(ea_, 5); {body} C.clk += 6;"
        elif mode == "izy":
            s = (f"{EA[mode]} if (px_) DRD({WRONG}, 4); "
                 f"uint8_t m_ = RD(ea_, 4 + px_); {body} C.clk += 5 + px_;")
        else:
            raise ValueError(o)
        return f"#define OPC_{o['op']:02X}(o_) do {{ {s} }} while (0)"
    if kind == "W":
        v = write_value(mn)
        pre = ""
        if mn in ("SHA", "SHX", "SHY", "TAS"):
            reg = {"SHA": "C.cpu.a & C.cpu.x", "SHX": "C.cpu.x", "SHY": "C.cpu.y",
                   "TAS": "C.cpu.sp"}[mn]
            if mn == "TAS":
                pre = "C.cpu.sp = (uint8_t)(C.cpu.a & C.cpu.x); "
            pre += (f"uint8_t v_ = (uint8_t)(({reg}) & (uint8_t)((b_ >> 8) + 1)); "
                    "uint16_t wa_ = px_ ? (uint16_t)((v_ << 8) | (ea_ & 0xFF)) : ea_; ")
            v = "v_"
            target = "wa_"
        else:
            target = "ea_"
        if mode in ("zp", "zpx", "zpy"):
            s = f"{EA[mode]} ZWR(ea_, {v}); C.clk += {cyc};"
        elif mode == "abs":
            s = f"{EA[mode]} WR(ea_, {v}, 3); C.clk += 4;"
        elif mode in ("abx", "aby"):
            s = f"{EA[mode]} {'' if pre else '(void)px_; '}DRD({WRONG}, 3); {pre}WR({target}, {v}, 4); C.clk += 5;"
        elif mode == "izx":
            s = f"{EA[mode]} WR(ea_, {v}, 5); C.clk += 6;"
        elif mode == "izy":
            s = f"{EA[mode]} {'' if pre else '(void)px_; '}DRD({WRONG}, 4); {pre}WR({target}, {v}, 5); C.clk += 6;"
        else:
            raise ValueError(o)
        return f"#define OPC_{o['op']:02X}(o_) do {{ {s} }} while (0)"
    if kind == "M":
        fn = RMW_FN[mn]
        if mode == "acc":
            return f"#define OPC_{o['op']:02X}() do {{ C.cpu.a = {fn}(C.cpu.a); C.clk += 2; }} while (0)"
        if mode in ("zp", "zpx"):
            s = f"{EA[mode]} uint8_t m_ = ZRD(ea_); m_ = {fn}(m_); ZWR(ea_, m_); C.clk += {cyc};"
        elif mode == "abs":
            s = (f"{EA[mode]} uint8_t m_ = RD(ea_, 3); DWR(ea_, m_, 4); "
                 f"m_ = {fn}(m_); WR(ea_, m_, 5); C.clk += 6;")
        elif mode in ("abx", "aby"):
            s = (f"{EA[mode]} (void)px_; DRD({WRONG}, 3); uint8_t m_ = RD(ea_, 4); "
                 f"DWR(ea_, m_, 5); m_ = {fn}(m_); WR(ea_, m_, 6); C.clk += 7;")
        elif mode == "izx":
            s = (f"{EA[mode]} uint8_t m_ = RD(ea_, 5); DWR(ea_, m_, 6); "
                 f"m_ = {fn}(m_); WR(ea_, m_, 7); C.clk += 8;")
        elif mode == "izy":
            s = (f"{EA[mode]} (void)px_; DRD({WRONG}, 4); uint8_t m_ = RD(ea_, 5); "
                 f"DWR(ea_, m_, 6); m_ = {fn}(m_); WR(ea_, m_, 7); C.clk += 8;")
        else:
            raise ValueError(o)
        return f"#define OPC_{o['op']:02X}(o_) do {{ {s} }} while (0)"
    return None


def main():
    out = []
    out.append("/* Generated by tools/gen_cpu_ops.py from tools/ljrecomp/opcodes.py.")
    out.append(" * Do not edit by hand. See runtime/cpu_ops.h for the helpers. */")
    out.append("#ifndef LJ_CPU_OPS_GEN_H")
    out.append("#define LJ_CPU_OPS_GEN_H")
    out.append("")
    for o in OPCODES:
        m = gen(o)
        if m:
            out.append(f"/* {o['op']:02X} {o['mn']} {o['mode']} */")
            out.append(m)
    out.append("")
    out.append("/* instruction length by opcode */")
    out.append("#define LJ_OPLEN_TABLE { \\")
    lens = [str(o["len"]) for o in OPCODES]
    for i in range(0, 256, 16):
        out.append("    " + ", ".join(lens[i:i + 16]) + ", \\")
    out.append("}")
    out.append("")
    out.append("#endif")
    print("\n".join(out))





def gen_interp_cases():
    """Switch cases for the interpreter (runtime/cpu_interp_gen.inc)."""
    out = ["/* Generated by tools/gen_cpu_ops.py. Do not edit by hand. */"]
    for o in OPCODES:
        if o["kind"] in ("F", "J"):
            continue
        op = o["op"]
        if o["len"] == 1:
            call = f"OPC_{op:02X}()"
        elif o["len"] == 2:
            call = f"OPC_{op:02X}(op1)"
        else:
            call = f"OPC_{op:02X}(op16)"
        out.append(f"case 0x{op:02X}: {call}; C.cpu.pc = (uint16_t)(pc + {o['len']}); break;")
    return "\n".join(out)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--interp":
        print(gen_interp_cases())
    else:
        main()
