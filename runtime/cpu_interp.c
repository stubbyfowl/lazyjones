/*
 * cpu_interp.c - reference 6510 interpreter and the CPU run loop.
 *
 * The interpreter runs any code that was not compiled ahead of time
 * (unknown code, code that the game modified at run time, the KERNAL
 * replacement). It uses the same instruction macros as the recompiled
 * code, so both paths behave the same.
 */
#include "cpu_ops.h"

static const uint8_t op_len[256] = LJ_OPLEN_TABLE;

#ifdef LJ_TRACE
#define TRACE_TARGET(pc) trace_mark_target(pc)
#else
#define TRACE_TARGET(pc) do { } while (0)
#endif

void cpu_reset_regs(void)
{
    C.cpu.a = C.cpu.x = C.cpu.y = 0;
    C.cpu.sp = 0xFF;
    cpu_set_p(0x24);
    C.cpu.jammed = 0;
}

static void cpu_branch(int cond, uint16_t pc, uint8_t off)
{
    uint16_t next = (uint16_t)(pc + 2);
    if (cond) {
        uint16_t t = (uint16_t)(next + (int8_t)off);
        C.clk += 3 + (((unsigned)(next ^ t) >> 8) & 1u);
        C.cpu.pc = t;
    } else {
        C.clk += 2;
        C.cpu.pc = next;
    }
}

static void cpu_jam(uint16_t pc, uint8_t op)
{
    C.cpu.jammed = 1;
    C.cpu.pc = pc;
    lj_logf("CPU jam: opcode $%02X at $%04X", op, pc);
}

void cpu_interp_step(void)
{
    uint16_t pc = C.cpu.pc;
    uint8_t op = RD(pc, 0);
    uint8_t op1 = 0;
    uint16_t op16 = 0;
    uint8_t len = op_len[op];
    if (len >= 2)
        op1 = RD((uint16_t)(pc + 1), 1);
    if (len == 3)
        op16 = (uint16_t)(op1 | (RD((uint16_t)(pc + 2), 2) << 8));
#ifdef LJ_TRACE
    if (lj_trace)
        trace_exec(pc, op, op1, (uint8_t)(op16 >> 8), len,
                   !(LJ_IS_PLAIN_RAM(pc) || rmap[pc >> 8] == &C.ram[pc & 0xFF00]));
#endif

    switch (op) {
#include "cpu_interp_gen.inc"

    case 0x00: { /* BRK */
        uint16_t ret = (uint16_t)(pc + 2);
        cpu_push((uint8_t)(ret >> 8));
        cpu_push((uint8_t)ret);
        cpu_push((uint8_t)(cpu_get_p() | 0x30));
        C.cpu.fi = 1;
        uint8_t lo = RD(0xFFFE, 5);
        uint8_t hi = RD(0xFFFF, 6);
        C.cpu.pc = (uint16_t)(lo | (hi << 8));
        C.clk += 7;
        break;
    }
    case 0x20: { /* JSR abs: the target high byte is fetched after the
                    * return address was pushed (matters when the stack
                    * overlaps the instruction) */
        uint16_t ret = (uint16_t)(pc + 2);
        cpu_push((uint8_t)(ret >> 8));
        cpu_push((uint8_t)ret);
        uint8_t hi = RD((uint16_t)(pc + 2), 5);
        C.cpu.pc = (uint16_t)(op1 | (hi << 8));
        C.clk += 6;
        break;
    }
    case 0x40: { /* RTI */
        cpu_set_p(cpu_pull());
        uint8_t lo = cpu_pull();
        uint8_t hi = cpu_pull();
        C.cpu.pc = (uint16_t)(lo | (hi << 8));
        C.clk += 6;
        cpu_irq_recheck_now();
        TRACE_TARGET(C.cpu.pc);
        break;
    }
    case 0x60: { /* RTS */
        uint8_t lo = cpu_pull();
        uint8_t hi = cpu_pull();
        C.cpu.pc = (uint16_t)((lo | (hi << 8)) + 1);
        C.clk += 6;
        TRACE_TARGET(C.cpu.pc);
        break;
    }
    case 0x4C: /* JMP abs */
        C.cpu.pc = op16;
        C.clk += 3;
        break;
    case 0x6C: { /* JMP (ind), with the NMOS page wrap */
        uint8_t lo = RD(op16, 3);
        uint8_t hi = RD((uint16_t)((op16 & 0xFF00) | ((op16 + 1) & 0x00FF)), 4);
        C.cpu.pc = (uint16_t)(lo | (hi << 8));
        C.clk += 5;
        TRACE_TARGET(C.cpu.pc);
        break;
    }
    case 0x10: cpu_branch(!(C.cpu.fn & 0x80), pc, op1); break; /* BPL */
    case 0x30: cpu_branch((C.cpu.fn & 0x80) != 0, pc, op1); break; /* BMI */
    case 0x50: cpu_branch(!C.cpu.fv, pc, op1); break; /* BVC */
    case 0x70: cpu_branch(C.cpu.fv, pc, op1); break; /* BVS */
    case 0x90: cpu_branch(!C.cpu.fc, pc, op1); break; /* BCC */
    case 0xB0: cpu_branch(C.cpu.fc, pc, op1); break; /* BCS */
    case 0xD0: cpu_branch(C.cpu.fz != 0, pc, op1); break; /* BNE */
    case 0xF0: cpu_branch(C.cpu.fz == 0, pc, op1); break; /* BEQ */

    case 0x02:
        /* $02 xx inside the KERNAL replacement ROM is a trap into C. */
        if ((pc >= 0xE000 && rmap[pc >> 8] == &hle_kernal[pc & 0x1F00]) ||
            (pc >= 0xA000 && pc < 0xC000 && rmap[pc >> 8] == &hle_basic[pc & 0x1F00])) {
            op1 = RD((uint16_t)(pc + 1), 1);
            C.cpu.pc = (uint16_t)(pc + 2);
            if (kernal_trap(op1))
                break;
        }
        cpu_jam(pc, op);
        break;
    default: /* remaining JAM opcodes */
        cpu_jam(pc, op);
        break;
    }
}

static void cpu_interrupt(uint16_t vector, int is_nmi)
{
    uint16_t pc = C.cpu.pc;
    cpu_push((uint8_t)(pc >> 8));
    cpu_push((uint8_t)pc);
    cpu_push((uint8_t)((cpu_get_p() & ~0x10) | 0x20));
    C.cpu.fi = 1;
    uint8_t lo = RD(vector, 5);
    uint8_t hi = RD((uint16_t)(vector + 1), 6);
    C.cpu.pc = (uint16_t)(lo | (hi << 8));
    C.clk += 7;
    TRACE_TARGET(C.cpu.pc);
    (void)is_nmi;
}

/* Called between instructions when an interrupt may be pending. An
 * interrupt is recognized when its line was active before the last cycle of
 * the previous instruction; otherwise one more instruction runs first. */
int cpu_take_interrupt(void)
{
    if (C.nmi_pending) {
        if (C.nmi_since + 2 > C.clk) {
            if (C.ev > C.clk + 1)
                C.ev = C.clk + 1;
            return 0;
        }
        C.nmi_pending = 0;
        cpu_interrupt(0xFFFA, 1);
        return 1;
    }
    if (C.irq_lines && !C.cpu.fi) {
        if (C.clk == C.irq_hold || C.irq_since + 2 > C.clk) {
            if (C.ev > C.clk + 1)
                C.ev = C.clk + 1;
            return 0;
        }
        cpu_interrupt(0xFFFE, 0);
        return 1;
    }
    return 0;
}

void machine_events(void);
extern int lj_recomp_enabled;

void cpu_run(uint64_t until)
{
    C.run_until = until;
    sched_update();
    while (C.clk < until) {
        if (C.clk >= C.ev)
            machine_events();
        if ((C.irq_lines && !C.cpu.fi) || C.nmi_pending) {
            if (cpu_take_interrupt())
                continue;
        }
        if (LJ_UNLIKELY(C.cpu.jammed)) {
            /* A jammed CPU only stops; devices keep running. */
            if (C.clk < C.ev)
                C.clk = C.ev;
            continue;
        }
        if (!(lj_recomp_enabled && recomp_run()))
            cpu_interp_step();
    }
}
