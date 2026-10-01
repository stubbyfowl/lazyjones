/*
 * c64.h - machine state for the Lazy Jones C64 runtime.
 *
 * The runtime models a PAL Commodore 64 (6510 CPU, 6569 VIC-II, 6581 SID,
 * two 6526 CIAs) closely enough to run the original game code. Game code
 * runs either as statically recompiled C (see tools/ljrecomp) or, for any
 * code the recompiler did not cover, through the reference interpreter.
 *
 * All state lives in one global struct (C) so that save states are a
 * single memcpy and the generated code can reach every field directly.
 */
#ifndef LJ_C64_H
#define LJ_C64_H

#include <stdint.h>
#include <stddef.h>
#include "lj.h"

#if defined(__GNUC__) || defined(__clang__)
#define LJ_LIKELY(x) __builtin_expect(!!(x), 1)
#define LJ_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define LJ_INLINE static inline __attribute__((always_inline))
#else
#define LJ_LIKELY(x) (x)
#define LJ_UNLIKELY(x) (x)
#define LJ_INLINE static inline
#endif

/* ---- PAL timing ------------------------------------------------------- */
#define C64_CLOCK_HZ 985248.0
#define VIC_LINES 312
#define VIC_CYCLES 63
#define C64_FRAME_CYCLES (VIC_LINES * VIC_CYCLES) /* 19656 */

/* ---- framebuffer -------------------------------------------------------
 * 384 x 272 palette indices (0..15). This is the usual "normal border"
 * PAL view: 32 pixels of border left/right of the 320 pixel display
 * window and 35/37 lines above/below the 200 line display window. */
#define FB_W 384
#define FB_H 272
#define FB_FIRST_LINE 16 /* raster line shown in framebuffer row 0 */
#define FB_P0 92         /* line pixel position (see vic.c) of fb column 0 */

/* ---- interrupt sources (bits in C.irq_lines) -------------------------- */
#define IRQ_SRC_VIC 0x01
#define IRQ_SRC_CIA1 0x02

/* ---- joystick bits (active high in the API) ---------------------------- */
#define JOY_UP 0x01
#define JOY_DOWN 0x02
#define JOY_LEFT 0x04
#define JOY_RIGHT 0x08
#define JOY_FIRE 0x10

typedef struct {
    uint8_t a, x, y, sp;
    uint16_t pc;
    uint8_t fn; /* N flag is bit 7 of fn */
    uint8_t fz; /* Z flag is set when fz == 0 */
    uint8_t fc, fv, fd, fi; /* 0 or 1 */
    uint8_t jammed;
} cpu_regs_t;

/* One CIA 6526 timer. The counter is evaluated lazily from the clock. */
typedef struct {
    uint16_t latch;
    uint16_t counter;   /* counter value at time 'base' */
    uint64_t base;      /* clock at which 'counter' was valid */
    uint8_t running;    /* counting phi2 cycles */
    uint8_t cascade;    /* timer B counting timer A underflows */
    uint64_t start_at;  /* counting begins at this clock (start delay) */
} cia_timer_t;

typedef struct {
    uint8_t pra, prb, ddra, ddrb;
    cia_timer_t ta, tb;
    uint8_t cra, crb;
    uint8_t icr_data;   /* latched interrupt flags (bits 0-4) */
    uint8_t icr_mask;   /* enabled interrupt sources */
    uint8_t irq;        /* IRQ/NMI output asserted */
    uint8_t sdr;
    /* time of day clock, BCD */
    uint8_t tod[4];     /* 10ths, sec, min, hr (bit 7 = PM) */
    uint8_t alarm[4];
    uint8_t tod_latch[4];
    uint8_t tod_latched, tod_halted;
    uint32_t tod_ticks; /* 50 Hz input ticks counted towards 1/10 s */
    uint64_t tod_next;  /* clock of the next 50 Hz input tick */
    uint64_t next_event;
} cia_t;

typedef struct {
    uint32_t acc;       /* 24 bit phase accumulator */
    uint32_t noise;     /* 23 bit noise LFSR */
    uint8_t prev_bit19;
    uint8_t env;        /* envelope counter 0..255 */
    uint8_t env_state;  /* 0 attack, 1 decay/sustain, 2 release */
    uint8_t hold_zero;
    uint16_t rate_cnt;
    uint16_t rate_period;
    uint8_t exp_cnt;
    uint8_t exp_period;
    uint8_t gate;
    uint16_t out12;     /* last waveform output (OSC3 readback) */
} sid_voice_t;

typedef struct {
    uint8_t regs[32];
    sid_voice_t v[3];
    uint64_t clk;       /* SID synthesized up to this clock */
    /* output generation */
    double cycles_per_sample;
    double sample_frac;
    float facc, fcnt;   /* sum and count of chip cycles of this sample */
    float lp, bp;       /* state variable filter */
    float dc_hp_x, dc_hp_y;
    uint8_t bus_value;  /* value last written (read of write-only regs) */
    uint64_t bus_clk;
} sid_t;

typedef struct {
    uint8_t regs[64];
    uint16_t line;          /* current raster line */
    uint64_t line_t0;       /* clock of cycle 1 of the current line */
    uint64_t frame_t0;      /* clock of cycle 1 of line 0 */
    uint8_t irr, imr;       /* $D019 latch (bits 0-3), $D01A */
    uint16_t raster_cmp;
    uint8_t den_frame;      /* DEN was set in line $30 */
    uint8_t badline;
    uint8_t display_state;
    uint16_t vc, vcbase;
    uint8_t rc;
    uint8_t vborder;        /* vertical border flip flop */
    uint8_t mborder;        /* main border flip flop */
    uint8_t cbuf[40];       /* screen codes fetched on the last bad line */
    uint8_t colbuf[40];     /* colors fetched on the last bad line */
    /* sprites */
    uint8_t mc[8], mcbase[8];
    uint8_t dma, disp;      /* bit per sprite */
    uint8_t yexp_ff;        /* bit per sprite */
    uint8_t sdata[8][3];    /* data for the current line */
    int16_t s_start[8];     /* pixel where the sprite started on this line */
    /* rendering */
    int16_t render_p;       /* pixels [0, render_p) of this line are done */
    uint8_t coll_ss, coll_sb;
    uint8_t cfetched;       /* c-accesses of this bad line done */
    uint16_t spr_stall;     /* sprite DMA cycles to steal at line start */
    uint8_t bl_stall;       /* bad line DMA pending (cycle 12) */
    uint64_t next_event;
    uint8_t frame_done;
} vic_t;

typedef struct {
    /* ---- CPU and memory ---- */
    cpu_regs_t cpu;
    uint64_t clk;           /* CPU clock (cycles since power on) */
    uint64_t ev;            /* earliest pending event: leave recompiled code */
    uint8_t irq_lines;      /* IRQ_SRC_* bits */
    uint64_t irq_since;     /* clock when the IRQ line was last asserted */
    uint8_t nmi_pending;
    uint64_t nmi_since;
    uint8_t nmi_line;
    uint64_t irq_hold;      /* clock right after CLI/PLP (IRQ sampled late) */
    uint64_t run_until;     /* end of the current cpu_run() slice */

    uint8_t port_ddr, port_data; /* 6510 I/O port ($00/$01) */
    uint8_t bank_cfg;       /* effective LORAM|HIRAM|CHAREN bits */

    uint8_t ram[65536];
    uint8_t colorram[1024];

    /* Self modifying code tracking (see mem.c). code_byte[a] is 1 when
     * address a is part of an instruction compiled ahead of time and
     * instr_mod[a] is non zero when the instruction that starts at a no
     * longer matches the bytes it was compiled from. */
    uint8_t code_byte[65536];
    uint8_t instr_mod[65536];

    vic_t vic;
    cia_t cia1, cia2;
    sid_t sid;

    /* ---- input ---- */
    uint8_t kb_matrix[8];   /* kb_matrix[col] bit r = key at (PA col, PB row) down */
    uint8_t joy1, joy2;     /* active high JOY_* bits */

    /* ---- frame output ---- */
    uint8_t fb[FB_H][FB_W];
    uint32_t frame_count;
} c64_t;

extern c64_t C;
extern const uint8_t *rmap[256]; /* readable page -> host pointer or NULL */
extern uint8_t *wmap[256];       /* writable page -> host pointer or NULL */
extern uint8_t hle_kernal[8192];
extern uint8_t hle_basic[8192];
extern uint8_t hle_chargen[4096];

/* ---- memory (mem.c) ---- */
void mem_init(void);
void mem_update_banks(void);
uint8_t mem_rd_slow(uint16_t addr, unsigned off);
void mem_wr_slow(uint16_t addr, uint8_t val, unsigned off);
void mem_wr_code(uint16_t addr, uint8_t val);
uint8_t mem_peek(uint16_t addr); /* CPU view, no side effects */
void mem_mark_code_bytes(uint16_t addr, unsigned len, int dyn_operand, const uint8_t *bytes);
void mem_recheck_all_code(void);
void mem_reset_code_marks(void);

/* ---- devices ---- */
void vic_reset(void);
uint8_t vic_read(uint16_t reg, uint64_t t);
void vic_write(uint16_t reg, uint8_t val, uint64_t t);
void vic_event(void);
uint8_t vic_peek(uint16_t reg);

void cia_reset(cia_t *c);
uint8_t cia_read(cia_t *c, int which, uint8_t reg, uint64_t t);
void cia_write(cia_t *c, int which, uint8_t reg, uint8_t val, uint64_t t);
void cia_event(cia_t *c, int which, uint64_t t);
uint8_t cia_peek(cia_t *c, int which, uint8_t reg);
uint8_t cia1_port_a_in(void);
uint8_t cia1_port_b_in(void);

void sid_reset(int sample_rate);
void sid_set_sample_rate(double rate);
uint8_t sid_read(uint8_t reg, uint64_t t);
void sid_write(uint8_t reg, uint8_t val, uint64_t t);
void sid_sync(uint64_t t);
void sid_post_load(void);
size_t sid_audio_read(int16_t *out, size_t max);
size_t sid_audio_available(void);

/* ---- scheduling (machine.c) ---- */
void sched_update(void);         /* recompute C.ev from device events */
void irq_set(uint8_t src, int on);
void nmi_set(int on);

/* ---- CPU (cpu_interp.c) ---- */
void cpu_reset_regs(void);
void cpu_interp_step(void);      /* executes one instruction */
int cpu_take_interrupt(void);    /* returns 1 if an interrupt was taken */
void cpu_run(uint64_t until);

/* Recompiled code entry point (generated). Returns 0 when C.cpu.pc is not
 * a recompiled instruction (the caller interprets one instruction). */
int recomp_run(void);
extern const int recomp_available;

/* ---- KERNAL replacement (kernal.c) ---- */
void kernal_build_rom(void);
int kernal_trap(uint8_t id);     /* returns 1 if handled */
void kernal_boot_state(void);

/* ---- diagnostics ---- */
void lj_logf(const char *fmt, ...);
extern int lj_debug_flags; /* bit 0: log CHROUT */

#endif
