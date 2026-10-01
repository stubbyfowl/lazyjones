/*
 * cpu_ops.h - 6510 instruction helpers shared by the interpreter and the
 * recompiled code. The per-opcode macros are generated into cpu_ops_gen.h.
 *
 * Timing model: C.clk holds the clock at the start of the instruction while
 * the instruction body runs. Every bus access passes its cycle offset within
 * the instruction (0 = opcode fetch), so I/O devices see the exact cycle of
 * the access. The macro adds the instruction's cycle count at the end.
 */
#ifndef LJ_CPU_OPS_H
#define LJ_CPU_OPS_H

#include "c64.h"
#include "trace.h"

#define SETNZ(v) (C.cpu.fn = C.cpu.fz = (uint8_t)(v))

/* ---- memory access ---------------------------------------------------- */

/* $0002-$9FFF is RAM for the CPU in every memory configuration this
 * runtime supports (no cartridge), so these addresses skip the page map. */
#define LJ_IS_PLAIN_RAM(a) ((a) >= 2u && (a) < 0xA000u)

LJ_INLINE uint8_t RD(uint16_t a, unsigned off)
{
    TRACE_READ(a);
    if (LJ_IS_PLAIN_RAM(a))
        return C.ram[a];
    const uint8_t *p = rmap[a >> 8];
    if (p)
        return p[a & 0xFF];
    return mem_rd_slow(a, off);
}

LJ_INLINE void WR(uint16_t a, uint8_t v, unsigned off)
{
    TRACE_WRITE(a);
    if (LJ_IS_PLAIN_RAM(a)) {
        if (LJ_UNLIKELY(C.code_byte[a]))
            mem_wr_code(a, v);
        else
            C.ram[a] = v;
        return;
    }
    uint8_t *p = wmap[a >> 8];
    if (p && !C.code_byte[a]) {
        p[a & 0xFF] = v;
        return;
    }
    mem_wr_slow(a, v, off);
}

/* Dummy read (indexed addressing page fix-up cycle). It only matters when
 * it hits an I/O register with read side effects. */
LJ_INLINE void DRD(uint16_t a, unsigned off)
{
    if (a >= 0xD000u && a < 0xE000u && !rmap[a >> 8])
        (void)mem_rd_slow(a, off);
}

/* Dummy write of the unmodified value during read-modify-write. */
LJ_INLINE void DWR(uint16_t a, uint8_t v, unsigned off)
{
    if (a >= 0xD000u && a < 0xE000u && !wmap[a >> 8])
        mem_wr_slow(a, v, off);
}

LJ_INLINE uint8_t ZRD(uint8_t a)
{
    TRACE_READ(a);
    if (LJ_UNLIKELY(a < 2))
        return mem_rd_slow(a, 0);
    return C.ram[a];
}

LJ_INLINE void ZWR(uint8_t a, uint8_t v)
{
    TRACE_WRITE(a);
    if (LJ_UNLIKELY(a < 2))
        mem_wr_slow(a, v, 0);
    else if (LJ_UNLIKELY(C.code_byte[a]))
        mem_wr_code(a, v);
    else
        C.ram[a] = v;
}

/* ---- status register and stack ---------------------------------------- */

LJ_INLINE uint8_t cpu_get_p(void)
{
    return (uint8_t)((C.cpu.fn & 0x80) | (C.cpu.fv ? 0x40 : 0) | 0x20 |
                     (C.cpu.fd ? 0x08 : 0) | (C.cpu.fi ? 0x04 : 0) |
                     (C.cpu.fz ? 0 : 0x02) | (C.cpu.fc ? 0x01 : 0));
}

LJ_INLINE void cpu_set_p(uint8_t p)
{
    C.cpu.fn = p;
    C.cpu.fv = (p >> 6) & 1;
    C.cpu.fd = (p >> 3) & 1;
    C.cpu.fi = (p >> 2) & 1;
    C.cpu.fz = (p & 0x02) ? 0 : 1;
    C.cpu.fc = p & 1;
}

LJ_INLINE void cpu_push(uint8_t v)
{
    uint16_t a = (uint16_t)(0x100 | C.cpu.sp);
    TRACE_WRITE(a);
    if (LJ_UNLIKELY(C.code_byte[a]))
        mem_wr_code(a, v);
    else
        C.ram[a] = v;
    C.cpu.sp--;
}

LJ_INLINE uint8_t cpu_pull(void)
{
    C.cpu.sp++;
    TRACE_READ((uint16_t)(0x100 | C.cpu.sp));
    return C.ram[0x100 | C.cpu.sp];
}

/* After CLI and PLP the interrupt flag is sampled one instruction late:
 * a pending IRQ is taken after the next instruction, not immediately. */
LJ_INLINE void cpu_irq_recheck_delayed(void)
{
    if (C.irq_lines && !C.cpu.fi) {
        C.irq_hold = C.clk;
        if (C.ev > C.clk + 1)
            C.ev = C.clk + 1;
    }
}

/* After RTI the restored flag is effective at once. */
LJ_INLINE void cpu_irq_recheck_now(void)
{
    if (C.irq_lines && !C.cpu.fi && C.ev > C.clk)
        C.ev = C.clk;
}

/* ---- ALU ---------------------------------------------------------------- */

LJ_INLINE void op_ora(uint8_t m) { C.cpu.a |= m; SETNZ(C.cpu.a); }
LJ_INLINE void op_and(uint8_t m) { C.cpu.a &= m; SETNZ(C.cpu.a); }
LJ_INLINE void op_eor(uint8_t m) { C.cpu.a ^= m; SETNZ(C.cpu.a); }

LJ_INLINE void op_adc(uint8_t m)
{
    unsigned a = C.cpu.a, c = C.cpu.fc;
    if (!C.cpu.fd) {
        unsigned s = a + m + c;
        C.cpu.fv = ((~(a ^ m) & (a ^ s)) & 0x80) != 0;
        C.cpu.fc = s > 0xFF;
        C.cpu.a = (uint8_t)s;
        SETNZ(C.cpu.a);
    } else {
        /* NMOS decimal mode: N and V from the intermediate result, Z from
         * the binary result (see 6502.org "Decimal Mode" appendix A). */
        unsigned bin = (a + m + c) & 0xFF;
        unsigned lo = (a & 0x0F) + (m & 0x0F) + c;
        if (lo >= 0x0A)
            lo = ((lo + 0x06) & 0x0F) + 0x10;
        unsigned s = (a & 0xF0) + (m & 0xF0) + lo;
        C.cpu.fn = (uint8_t)(s & 0x80);
        C.cpu.fv = ((~(a ^ m) & (a ^ s)) & 0x80) != 0;
        if (s >= 0xA0)
            s += 0x60;
        C.cpu.fc = s >= 0x100;
        C.cpu.a = (uint8_t)s;
        C.cpu.fz = (uint8_t)bin;
    }
}

LJ_INLINE void op_sbc(uint8_t m)
{
    unsigned a = C.cpu.a, c = C.cpu.fc;
    int bin = (int)a - (int)m - (int)(1 - c);
    C.cpu.fv = (((a ^ m) & (a ^ (unsigned)bin)) & 0x80) != 0;
    C.cpu.fc = bin >= 0;
    SETNZ(bin);
    if (!C.cpu.fd) {
        C.cpu.a = (uint8_t)bin;
    } else {
        /* NMOS decimal mode: flags are the binary flags. */
        int lo = (int)(a & 0x0F) - (int)(m & 0x0F) + (int)c - 1;
        if (lo < 0)
            lo = ((lo - 0x06) & 0x0F) - 0x10;
        int r = (int)(a & 0xF0) - (int)(m & 0xF0) + lo;
        if (r < 0)
            r -= 0x60;
        C.cpu.a = (uint8_t)r;
    }
}

LJ_INLINE void op_cmp(uint8_t r, uint8_t m)
{
    C.cpu.fc = r >= m;
    SETNZ(r - m);
}

LJ_INLINE void op_bit(uint8_t m)
{
    C.cpu.fn = m;
    C.cpu.fv = (m >> 6) & 1;
    C.cpu.fz = (uint8_t)(C.cpu.a & m);
}

LJ_INLINE uint8_t op_asl(uint8_t m) { C.cpu.fc = m >> 7; m = (uint8_t)(m << 1); SETNZ(m); return m; }
LJ_INLINE uint8_t op_lsr(uint8_t m) { C.cpu.fc = m & 1; m >>= 1; SETNZ(m); return m; }
LJ_INLINE uint8_t op_rol(uint8_t m)
{
    uint8_t c = C.cpu.fc;
    C.cpu.fc = m >> 7;
    m = (uint8_t)((m << 1) | c);
    SETNZ(m);
    return m;
}
LJ_INLINE uint8_t op_ror(uint8_t m)
{
    uint8_t c = C.cpu.fc;
    C.cpu.fc = m & 1;
    m = (uint8_t)((m >> 1) | (c << 7));
    SETNZ(m);
    return m;
}
LJ_INLINE uint8_t op_inc(uint8_t m) { m++; SETNZ(m); return m; }
LJ_INLINE uint8_t op_dec(uint8_t m) { m--; SETNZ(m); return m; }
LJ_INLINE uint8_t op_slo(uint8_t m) { m = op_asl(m); op_ora(m); return m; }
LJ_INLINE uint8_t op_rla(uint8_t m) { m = op_rol(m); op_and(m); return m; }
LJ_INLINE uint8_t op_sre(uint8_t m) { m = op_lsr(m); op_eor(m); return m; }
LJ_INLINE uint8_t op_rra(uint8_t m) { m = op_ror(m); op_adc(m); return m; }
LJ_INLINE uint8_t op_dcp(uint8_t m) { m--; op_cmp(C.cpu.a, m); return m; }
LJ_INLINE uint8_t op_isc(uint8_t m) { m++; op_sbc(m); return m; }

/* ARR (#$6B): AND then ROR with special flags; decimal variant per the
 * "No More Secrets" description of the NMOS 6510. */
LJ_INLINE void op_arr(uint8_t m)
{
    uint8_t t = (uint8_t)(C.cpu.a & m);
    uint8_t r = (uint8_t)((t >> 1) | (C.cpu.fc << 7));
    if (!C.cpu.fd) {
        SETNZ(r);
        C.cpu.fc = (r >> 6) & 1;
        C.cpu.fv = ((r >> 6) ^ (r >> 5)) & 1;
        C.cpu.a = r;
    } else {
        C.cpu.fn = (uint8_t)(C.cpu.fc << 7);
        C.cpu.fz = r;
        C.cpu.fv = ((t ^ r) >> 6) & 1;
        if ((t & 0x0F) + (t & 0x01) > 5)
            r = (uint8_t)((r & 0xF0) | ((r + 6) & 0x0F));
        if ((unsigned)(t & 0xF0) + (t & 0x10) > 0x50) {
            r = (uint8_t)(r + 0x60);
            C.cpu.fc = 1;
        } else {
            C.cpu.fc = 0;
        }
        C.cpu.a = r;
    }
}

#include "cpu_ops_gen.h"

#endif
