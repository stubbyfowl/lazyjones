/*
 * cpu_klaus.c - runs Klaus Dormann's 6502 functional test on the runtime's
 * CPU core (interpreter and instruction macros).
 *
 *   cpu_klaus 6502_functional_test.bin [success_addr_hex]
 *
 * The test image is loaded into all-RAM memory (port $01 = $30) and started
 * at $0400. The test ends in a "JMP *" loop: at the success address when
 * all tests pass, anywhere else on failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/c64.h"
#include "../runtime/cpu_ops.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s image.bin [success_hex]\n", argv[0]);
        return 2;
    }
    unsigned success = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 16) : 0x3469;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static uint8_t img[65536];
    size_t n = fread(img, 1, sizeof img, f);
    fclose(f);
    lj_power_on(48000);
    memcpy(C.ram, img, n);
    /* all RAM: no ROM, no I/O */
    C.port_ddr = 0x07;
    C.port_data = 0x00;
    mem_update_banks();
    C.cpu.pc = 0x0400;
    C.cpu.sp = 0xFF;
    uint64_t steps = 0;
    uint16_t last = 0xFFFF;
    for (;;) {
        uint16_t pc = C.cpu.pc;
        cpu_interp_step();
        steps++;
        if (C.cpu.pc == pc && pc == last)
            break;
        last = pc;
        if (steps > 200000000ULL) { fprintf(stderr, "timeout\n"); return 1; }
    }
    printf("stopped at $%04X after %llu instructions, %llu cycles\n", C.cpu.pc,
           (unsigned long long)steps, (unsigned long long)C.clk);
    if (C.cpu.pc == success) { printf("PASS\n"); return 0; }
    printf("FAIL\n");
    return 1;
}
