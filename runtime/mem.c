/*
 * mem.c - C64 memory map: 6510 port banking, I/O dispatch and tracking of
 * writes into recompiled code (self modifying code).
 */
#include <string.h>
#include "cpu_ops.h"

const uint8_t *rmap[256];
uint8_t *wmap[256];
uint8_t hle_kernal[8192];
uint8_t hle_basic[8192];
uint8_t hle_chargen[4096];

/* For every byte that belongs to a compiled instruction: the value the
 * instruction was compiled from. instr_chk[a] is the number of bytes of the
 * instruction at a that the compiled code depends on (0 = no compiled
 * instruction starts at a). */
static uint8_t code_expect[65536];
static uint8_t instr_chk[65536];

void mem_update_banks(void)
{
    uint8_t eff = (uint8_t)((C.port_data | (uint8_t)~C.port_ddr) & 0x07);
    int loram = eff & 1, hiram = (eff >> 1) & 1, charen = (eff >> 2) & 1;
    C.bank_cfg = eff;
    for (int p = 0; p < 256; p++) {
        rmap[p] = &C.ram[p << 8];
        wmap[p] = &C.ram[p << 8];
    }
    /* page 0 holds the 6510 port at $00/$01: always use the slow path */
    rmap[0] = NULL;
    wmap[0] = NULL;
    if (loram && hiram) {
        for (int p = 0xA0; p < 0xC0; p++)
            rmap[p] = &hle_basic[(p - 0xA0) << 8];
    }
    if (loram || hiram) {
        for (int p = 0xD0; p < 0xE0; p++) {
            if (charen) {
                rmap[p] = NULL;
                wmap[p] = NULL;
            } else {
                rmap[p] = &hle_chargen[((p - 0xD0) & 0x0F) << 8];
            }
        }
    }
    if (hiram) {
        for (int p = 0xE0; p < 0x100; p++)
            rmap[p] = &hle_kernal[(p - 0xE0) << 8];
    }
}

/* set by the CPU test harness: $00/$01 are plain RAM, no banking */
int lj_flat_ram_test;

static uint8_t port_read(uint16_t a)
{
    if (lj_flat_ram_test)
        return C.ram[a];
    if (a == 0)
        return C.port_ddr;
    /* inputs: LORAM/HIRAM/CHAREN pulled up, cassette sense high */
    return (uint8_t)((C.port_data & C.port_ddr) | ((uint8_t)~C.port_ddr & 0x17));
}

static uint8_t io_read(uint16_t a, uint64_t t)
{
    switch ((a >> 8) & 0x0F) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        return vic_read(a & 0x3F, t);
    case 0x4: case 0x5: case 0x6: case 0x7:
        return sid_read(a & 0x1F, t);
    case 0x8: case 0x9: case 0xA: case 0xB:
        /* colour RAM is 4 bits wide; the upper nibble floats */
        return (uint8_t)((C.colorram[a & 0x3FF] & 0x0F) | 0xF0);
    case 0xC:
        return cia_read(&C.cia1, 1, a & 0x0F, t);
    case 0xD:
        return cia_read(&C.cia2, 2, a & 0x0F, t);
    default:
        return 0xFF; /* expansion port I/O 1/2: nothing connected */
    }
}

static void io_write(uint16_t a, uint8_t v, uint64_t t)
{
    switch ((a >> 8) & 0x0F) {
    case 0x0: case 0x1: case 0x2: case 0x3:
        vic_write(a & 0x3F, v, t);
        break;
    case 0x4: case 0x5: case 0x6: case 0x7:
        sid_write(a & 0x1F, v, t);
        break;
    case 0x8: case 0x9: case 0xA: case 0xB:
        C.colorram[a & 0x3FF] = v & 0x0F;
        break;
    case 0xC:
        cia_write(&C.cia1, 1, a & 0x0F, v, t);
        break;
    case 0xD:
        cia_write(&C.cia2, 2, a & 0x0F, v, t);
        break;
    default:
        break;
    }
}

uint8_t mem_rd_slow(uint16_t a, unsigned off)
{
    if (a < 2)
        return port_read(a);
    if (a < 0x100)
        return C.ram[a];
    const uint8_t *p = rmap[a >> 8];
    if (p)
        return p[a & 0xFF];
    return io_read(a, C.clk + off);
}

/* Re-evaluate whether the compiled instruction at s still matches memory. */
static void recheck_instr(uint16_t s)
{
    uint8_t n = instr_chk[s];
    uint8_t mod = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint16_t b = (uint16_t)(s + i);
        if (C.ram[b] != code_expect[b])
            mod = 1;
    }
    C.instr_mod[s] = mod;
}

void mem_wr_code(uint16_t a, uint8_t v)
{
    if (C.ram[a] == v)
        return;
    C.ram[a] = v;
    for (int d = 0; d < 3; d++) {
        uint16_t s = (uint16_t)(a - d);
        if (instr_chk[s] > d)
            recheck_instr(s);
    }
    /* leave compiled code so the next instruction is validated */
    if (C.ev > C.clk)
        C.ev = C.clk;
}

void mem_wr_slow(uint16_t a, uint8_t v, unsigned off)
{
    if (a < 2 && lj_flat_ram_test) {
        C.ram[a] = v;
        return;
    }
    if (a < 2) {
        if (a == 0)
            C.port_ddr = v;
        else
            C.port_data = v;
        /* the CPU also drives the value onto the bus: RAM at $00/$01 */
        C.ram[a] = v;
        uint8_t eff = (uint8_t)((C.port_data | (uint8_t)~C.port_ddr) & 0x07);
        if (eff != C.bank_cfg)
            mem_update_banks();
        return;
    }
    uint8_t *p = wmap[a >> 8];
    if (p) {
        if (C.code_byte[a])
            mem_wr_code(a, v);
        else
            p[a & 0xFF] = v;
        return;
    }
    io_write(a, v, C.clk + off);
}

uint8_t mem_peek(uint16_t a)
{
    if (a < 2)
        return port_read(a);
    const uint8_t *p = rmap[a >> 8];
    if (a < 0x100 || p == NULL) {
        if (a < 0x100)
            return C.ram[a];
        switch ((a >> 8) & 0x0F) {
        case 0x0: case 0x1: case 0x2: case 0x3:
            return vic_peek(a & 0x3F);
        case 0x8: case 0x9: case 0xA: case 0xB:
            return (uint8_t)((C.colorram[a & 0x3FF] & 0x0F) | 0xF0);
        case 0xC:
            return cia_peek(&C.cia1, 1, a & 0x0F);
        case 0xD:
            return cia_peek(&C.cia2, 2, a & 0x0F);
        default:
            return 0xFF;
        }
    }
    return p[a & 0xFF];
}

void mem_reset_code_marks(void)
{
    memset(C.code_byte, 0, sizeof C.code_byte);
    memset(C.instr_mod, 0, sizeof C.instr_mod);
    memset(code_expect, 0, sizeof code_expect);
    memset(instr_chk, 0, sizeof instr_chk);
}

/* Register a compiled instruction. bytes[] are the instruction bytes the
 * code was generated from. With dyn_operand the compiled code reads its
 * operand from memory at run time, so only the opcode byte is checked. */
void mem_mark_code_bytes(uint16_t addr, unsigned len, int dyn_operand, const uint8_t *bytes)
{
    unsigned chk = dyn_operand ? 1 : len;
    if (instr_chk[addr] < chk)
        instr_chk[addr] = (uint8_t)chk;
    for (unsigned i = 0; i < chk; i++) {
        uint16_t b = (uint16_t)(addr + i);
        code_expect[b] = bytes[i];
        C.code_byte[b] = 1;
    }
}

/* Recompute instr_mod for all compiled instructions (after loading RAM). */
void mem_recheck_all_code(void)
{
    for (unsigned s = 0; s < 65536; s++)
        if (instr_chk[s])
            recheck_instr((uint16_t)s);
}

void mem_init(void)
{
    memset(C.ram, 0, sizeof C.ram);
    memset(C.colorram, 0, sizeof C.colorram);
    C.port_ddr = 0x2F;
    C.port_data = 0x37;
    C.ram[0] = C.port_ddr;
    C.ram[1] = C.port_data;
    C.bank_cfg = 0xFF;
    mem_update_banks();
}
