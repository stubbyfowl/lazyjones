/*
 * cpu_ss.c - runs the SingleStepTests 6502 vectors (converted by
 * ss_convert.py) through the runtime's CPU core and compares registers,
 * memory and cycle counts.  Usage: cpu_ss DIR [opcode_hex]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/c64.h"
#include "../runtime/cpu_ops.h"

extern int lj_flat_ram_test;

typedef struct { uint16_t pc; uint8_t s, a, x, y, p, n; uint16_t addr[64]; uint8_t val[64]; } st_t;

static int rd_state(FILE *f, st_t *st)
{
    uint8_t h[8];
    if (fread(h, 1, 8, f) != 8) return 0;
    st->pc = (uint16_t)(h[0] | (h[1] << 8));
    st->s = h[2]; st->a = h[3]; st->x = h[4]; st->y = h[5]; st->p = h[6]; st->n = h[7];
    for (int i = 0; i < st->n; i++) {
        uint8_t r[3];
        if (fread(r, 1, 3, f) != 3) return 0;
        st->addr[i] = (uint16_t)(r[0] | (r[1] << 8));
        st->val[i] = r[2];
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *dir = argv[1];
    int only = argc > 2 ? (int)strtol(argv[2], NULL, 16) : -1;
    lj_power_on(48000);
    lj_flat_ram_test = 1;
    C.port_ddr = 0x07; C.port_data = 0x00;
    mem_update_banks();
    rmap[0] = &C.ram[0]; wmap[0] = &C.ram[0];
    long total_fail = 0, total = 0;
    long skipped = 0;
    for (int op = 0; op < 256; op++) {
        if (only >= 0 && op != only) continue;
        /* JAM opcodes halt the CPU (here: PC stays on the opcode); the test
         * data expects PC + 1 after them, so they are not compared */
        if ((op & 0x0F) == 0x02 && op != 0x82 && op != 0xA2 && op != 0xC2 && op != 0xE2) {
            skipped++;
            continue;
        }
        char path[512];
        snprintf(path, sizeof path, "%s/%02x.bin", dir, op);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        st_t in, out;
        long n = 0, fail = 0, cycfail = 0;
        char first[256] = "";
        while (rd_state(f, &in) && rd_state(f, &out)) {
            uint8_t ncyc;
            if (fread(&ncyc, 1, 1, f) != 1) break;
            n++;
            for (int i = 0; i < in.n; i++) C.ram[in.addr[i]] = in.val[i];
            C.cpu.pc = in.pc; C.cpu.sp = in.s; C.cpu.a = in.a; C.cpu.x = in.x; C.cpu.y = in.y;
            cpu_set_p(in.p);
            C.cpu.jammed = 0;
            uint64_t c0 = C.clk;
            cpu_interp_step();
            uint64_t cyc = C.clk - c0;
            int bad = 0;
            char why[256] = "";
            uint8_t p = (uint8_t)(cpu_get_p() | 0x10);
            if (C.cpu.pc != out.pc || C.cpu.sp != out.s || C.cpu.a != out.a || C.cpu.x != out.x ||
                C.cpu.y != out.y || (p | 0x30) != (out.p | 0x30)) {
                bad = 1;
                snprintf(why, sizeof why, "regs pc %04x/%04x s %02x/%02x a %02x/%02x x %02x/%02x y %02x/%02x p %02x/%02x",
                         C.cpu.pc, out.pc, C.cpu.sp, out.s, C.cpu.a, out.a, C.cpu.x, out.x, C.cpu.y, out.y, p, out.p);
            }
            for (int i = 0; i < out.n && !bad; i++)
                if (C.ram[out.addr[i]] != out.val[i]) {
                    bad = 1;
                    snprintf(why, sizeof why, "mem %04x = %02x want %02x", out.addr[i], C.ram[out.addr[i]], out.val[i]);
                }
            if (!C.cpu.jammed && cyc != ncyc) {
                cycfail++;
                if (!bad) snprintf(why, sizeof why, "cycles %llu want %u", (unsigned long long)cyc, ncyc);
                bad = 1;
            }
            if (bad) {
                if (!fail) snprintf(first, sizeof first, "%s", why);
                fail++;
            }
            for (int i = 0; i < in.n; i++) C.ram[in.addr[i]] = 0;
            for (int i = 0; i < out.n; i++) C.ram[out.addr[i]] = 0;
        }
        fclose(f);
        total += n;
        total_fail += fail;
        if (fail)
            printf("%02X: %ld/%ld fail (cycles %ld) first: %s\n", op, fail, n, cycfail, first);
    }
    printf("total %ld tests, %ld failures (%ld JAM opcodes not compared)\n", total, total_fail, skipped);
    return total_fail != 0;
}
