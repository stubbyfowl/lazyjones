/*
 * machine.c - power on, scheduling between devices, and the frame API used
 * by the platform front ends.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cpu_ops.h"
#include "lj.h"

c64_t C;

static lj_log_fn log_fn;

void lj_set_log(lj_log_fn fn) { log_fn = fn; }

void lj_logf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log_fn)
        log_fn(buf);
}

void irq_set(uint8_t src, int on)
{
    uint8_t old = C.irq_lines;
    if (on)
        C.irq_lines |= src;
    else
        C.irq_lines &= (uint8_t)~src;
    if (!old && C.irq_lines) {
        C.irq_since = C.clk;
        if (C.ev > C.clk)
            C.ev = C.clk;
    }
}

void nmi_set(int on)
{
    if (on && !C.nmi_line) {
        C.nmi_pending = 1;
        C.nmi_since = C.clk;
        if (C.ev > C.clk)
            C.ev = C.clk;
    }
    C.nmi_line = (uint8_t)(on != 0);
}

void sched_update(void)
{
    uint64_t e = C.run_until;
    if (C.vic.next_event < e)
        e = C.vic.next_event;
    if (C.cia1.next_event < e)
        e = C.cia1.next_event;
    if (C.cia2.next_event < e)
        e = C.cia2.next_event;
    C.ev = e;
}

void machine_events(void)
{
    vic_event();
    cia_event(&C.cia1, 1, C.clk);
    cia_event(&C.cia2, 2, C.clk);
    sched_update();
}

/* ---- public API ---------------------------------------------------------- */

void lj_power_on(int sample_rate)
{
    memset(&C, 0, sizeof C);
    mem_init();
    mem_reset_code_marks();
    kernal_build_rom();
    cpu_reset_regs();
    vic_reset();
    cia_reset(&C.cia1);
    cia_reset(&C.cia2);
    sid_reset(sample_rate);
    C.run_until = 0;
    sched_update();
}

int lj_load_prg(const uint8_t *data, size_t len)
{
    if (len < 3)
        return -1;
    uint16_t la = (uint16_t)(data[0] | (data[1] << 8));
    size_t n = len - 2;
    if ((size_t)la + n > 0x10000)
        return -1;
    memcpy(&C.ram[la], data + 2, n);
    /* KERNAL LOAD sets the end address; BASIC uses it as start of
     * variables (VARTAB) after a load into the BASIC area */
    uint16_t end = (uint16_t)(la + n);
    C.ram[0xAE] = (uint8_t)end;
    C.ram[0xAF] = (uint8_t)(end >> 8);
    if (la == 0x0801) {
        C.ram[0x2D] = C.ram[0x2F] = C.ram[0x31] = (uint8_t)end;
        C.ram[0x2E] = C.ram[0x30] = C.ram[0x32] = (uint8_t)(end >> 8);
    }
    return 0;
}

void lj_start(uint16_t entry)
{
    kernal_boot_state();
    /* BASIC SYS: registers from $030C-$030F, return address on the stack */
    C.cpu.a = C.ram[0x030C];
    C.cpu.x = C.ram[0x030D];
    C.cpu.y = C.ram[0x030E];
    cpu_set_p((uint8_t)(C.ram[0x030F] | 0x20));
    C.cpu.pc = entry;
    mem_recheck_all_code();
}

void lj_run_frame(void)
{
    if (C.clk >= C.ev)
        machine_events();
    C.vic.frame_done = 0;
    uint64_t until = C.vic.frame_t0 + C64_FRAME_CYCLES;
    cpu_run(until);
    /* finish the frame: process the wrap to line 0 */
    C.run_until = C.clk + 1;
    sched_update();
    if (C.clk >= C.ev)
        machine_events();
    sid_sync(C.clk);
}

const uint8_t *lj_framebuffer(void) { return &C.fb[0][0]; }

void lj_set_joystick(int port, uint8_t bits)
{
    if (port == 1)
        C.joy1 = bits & 0x1F;
    else
        C.joy2 = bits & 0x1F;
}

void lj_set_key(int col, int row, int down)
{
    if (col < 0 || col > 7 || row < 0 || row > 7)
        return;
    if (down)
        C.kb_matrix[col] |= (uint8_t)(1 << row);
    else
        C.kb_matrix[col] &= (uint8_t)~(1 << row);
}

void lj_clear_keys(void) { memset(C.kb_matrix, 0, sizeof C.kb_matrix); }

size_t lj_audio_read(int16_t *out, size_t max) { return sid_audio_read(out, max); }

void lj_set_sample_rate(double rate) { sid_set_sample_rate(rate); }

size_t lj_state_size(void) { return sizeof C; }

void lj_state_save(void *buf) { memcpy(buf, &C, sizeof C); }

int lj_state_load(const void *buf, size_t len)
{
    if (len != sizeof C)
        return -1;
    memcpy(&C, buf, sizeof C);
    mem_update_banks();
    sid_post_load();
    return 0;
}

uint64_t lj_clock(void) { return C.clk; }
const uint8_t *lj_debug_ram(void) { return C.ram; }
uint32_t lj_frame_count(void) { return C.frame_count; }
