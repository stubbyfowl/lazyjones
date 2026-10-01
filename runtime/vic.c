/*
 * vic.c - MOS 6569 (PAL VIC-II) emulation.
 *
 * Model
 *  - Every raster line has 63 cycles. Cycle 1 of line L starts at clock
 *    frame_t0 + 63 * L. The pixel position within a line is
 *    P = 8 * (cycle - 1) + pixel, 0..503, and X coordinate (sprite X)
 *    maps to P = (X - 0x194) mod 0x1F8. The display window starts at
 *    X = 24, i.e. P = 124.
 *  - Lines are rendered lazily: before every register write or collision
 *    read the line is rendered up to the current pixel, so mid-line
 *    register changes (colours, modes, borders) land on the right pixel.
 *  - The sequencer state machine (VC, VCBASE, RC, display/idle state,
 *    bad lines, sprite DMA with MC/MCBASE and the Y expansion flip flop,
 *    border flip flops) follows "The MOS 6567/6569 video controller
 *    (VIC-II) and its application in the Commodore 64" by C. Bauer.
 *  - Cycles stolen from the CPU by bad lines and sprite DMA are added to
 *    the CPU clock between instructions.
 */
#include <string.h>
#include "cpu_ops.h"

#define V C.vic
#define P_DISPLAY 124 /* P of X coordinate 24 */
#define LINE_PIXELS 504

/* raster line -> framebuffer row, pixel P -> framebuffer column */
#define FB_ROW(line) ((int)(line) - FB_FIRST_LINE)

/* scratch buffers for the line being rendered */
static uint8_t g_col[LINE_PIXELS];
static uint8_t g_fg[LINE_PIXELS];
static uint8_t s_col[LINE_PIXELS];
static uint8_t s_mask[LINE_PIXELS];
static uint8_t s_prio[LINE_PIXELS];

static inline uint16_t vic_bank_base(void)
{
    cia_t *c = &C.cia2;
    uint8_t pa = (uint8_t)(c->pra | (uint8_t)~c->ddra);
    return (uint16_t)((3 - (pa & 3)) << 14);
}

/* 14 bit VIC address -> byte, with the character ROM image visible at
 * $1000-$1FFF of banks 0 and 2. */
static inline uint8_t vic_mem(uint16_t base, uint16_t a14)
{
    a14 &= 0x3FFF;
    if ((a14 & 0x3000) == 0x1000 && !(base & 0x4000))
        return hle_chargen[a14 & 0x0FFF];
    return C.ram[(uint16_t)(base | a14)];
}

static void vic_update_irq(uint64_t t)
{
    int on = (V.irr & V.imr & 0x0F) != 0;
    (void)t;
    irq_set(IRQ_SRC_VIC, on);
}

static void vic_raise(uint8_t bit, uint64_t t)
{
    V.irr |= bit;
    if (V.imr & bit) {
        if (!(C.irq_lines & IRQ_SRC_VIC)) {
            irq_set(IRQ_SRC_VIC, 1);
            C.irq_since = t;
        }
    }
}

static int badline_cond(uint16_t line)
{
    return V.den_frame && line >= 0x30 && line <= 0xF7 &&
           (line & 7) == (V.regs[0x11] & 7);
}

static void cfetch(void)
{
    if (V.cfetched)
        return;
    V.cfetched = 1;
    uint16_t base = vic_bank_base();
    uint16_t vm = (uint16_t)((V.regs[0x18] & 0xF0) << 6);
    for (int i = 0; i < 40; i++) {
        uint16_t a = (uint16_t)((V.vc + i) & 0x3FF);
        V.cbuf[i] = vic_mem(base, (uint16_t)(vm | a));
        V.colbuf[i] = C.colorram[a] & 0x0F;
    }
}

/* ---- rendering ---------------------------------------------------------- */

static void render_graphics(int p0, int p1)
{
    uint8_t d011 = V.regs[0x11], d016 = V.regs[0x16], d018 = V.regs[0x18];
    int ecm = (d011 >> 6) & 1, bmm = (d011 >> 5) & 1, mcm = (d016 >> 4) & 1;
    int xs = d016 & 7;
    uint8_t b0c = V.regs[0x21] & 15;
    int first = P_DISPLAY + xs, last = P_DISPLAY + 320 + xs;
    uint16_t base = vic_bank_base();

    if (V.vborder) {
        /* the sequencer is switched off inside the vertical border */
        memset(&g_col[p0], b0c, (size_t)(p1 - p0));
        memset(&g_fg[p0], 0, (size_t)(p1 - p0));
        return;
    }
    if (V.badline && V.display_state && p1 > first)
        cfetch();

    int p = p0;
    while (p < p1) {
        if (p < first || p >= last) {
            g_col[p] = b0c;
            g_fg[p] = 0;
            p++;
            continue;
        }
        int i = (p - first) >> 3;
        int bit0 = (p - first) & 7;
        uint8_t c, col, g;
        if (V.display_state) {
            c = V.cbuf[i];
            col = V.colbuf[i];
            uint16_t a;
            if (bmm)
                a = (uint16_t)(((d018 & 0x08) << 10) | (((V.vc + i) & 0x3FF) << 3) | V.rc);
            else
                a = (uint16_t)(((d018 & 0x0E) << 10) | ((ecm ? (c & 0x3F) : c) << 3) | V.rc);
            if (ecm)
                a &= 0x39FF;
            g = vic_mem(base, a);
        } else {
            c = 0;
            col = 0;
            g = vic_mem(base, ecm ? 0x39FF : 0x3FFF);
        }
        int n = 8 - bit0;
        if (p + n > p1)
            n = p1 - p;
        for (int k = bit0; k < bit0 + n; k++, p++) {
            uint8_t pix, fg;
            int multi = mcm && (bmm || (col & 8));
            if (!multi) {
                int b = (g >> (7 - k)) & 1;
                fg = (uint8_t)b;
                if (bmm)
                    pix = b ? (c >> 4) : (c & 15);
                else if (ecm)
                    pix = b ? col : (V.regs[0x21 + (c >> 6)] & 15);
                else
                    pix = b ? (uint8_t)(mcm ? (col & 7) : col) : b0c;
            } else {
                int b = (g >> (6 - (k & 6))) & 3;
                fg = (uint8_t)(b >= 2);
                if (bmm) {
                    switch (b) {
                    case 0: pix = b0c; break;
                    case 1: pix = c >> 4; break;
                    case 2: pix = c & 15; break;
                    default: pix = col; break;
                    }
                } else {
                    switch (b) {
                    case 0: pix = b0c; break;
                    case 1: pix = V.regs[0x22] & 15; break;
                    case 2: pix = V.regs[0x23] & 15; break;
                    default: pix = col & 7; break;
                    }
                }
            }
            if (ecm && (bmm || mcm))
                pix = 0; /* invalid modes show black */
            g_col[p] = pix;
            g_fg[p] = fg;
        }
    }
}

static void render_sprites(int p0, int p1)
{
    memset(&s_mask[p0], 0, (size_t)(p1 - p0));
    if (!V.disp)
        return;
    for (int s = 7; s >= 0; s--) {
        uint8_t bit = (uint8_t)(1 << s);
        if (!(V.disp & bit))
            continue;
        int start = V.s_start[s];
        if (start < 0) {
            int x = V.regs[2 * s] | ((V.regs[0x10] & bit) ? 0x100 : 0);
            if (x >= 0x1F8)
                continue; /* never matches the X counter */
            int ps = (x - 0x194 + 0x1F8) % 0x1F8;
            if (ps >= p1)
                continue; /* not reached yet */
            if (ps < p0)
                continue; /* X was moved behind the beam on this line */
            V.s_start[s] = (int16_t)ps;
            start = ps;
        }
        int xexp = (V.regs[0x1D] >> s) & 1;
        int mc = (V.regs[0x1C] >> s) & 1;
        int width = xexp ? 48 : 24;
        int a = start > p0 ? start : p0;
        int b = start + width < p1 ? start + width : p1;
        if (a >= b)
            continue;
        uint32_t d = ((uint32_t)V.sdata[s][0] << 16) | ((uint32_t)V.sdata[s][1] << 8) | V.sdata[s][2];
        uint8_t scol = V.regs[0x27 + s] & 15;
        uint8_t mm0 = V.regs[0x25] & 15, mm1 = V.regs[0x26] & 15;
        uint8_t prio = (V.regs[0x1B] >> s) & 1;
        for (int p = a; p < b; p++) {
            int k = p - start;
            if (xexp)
                k >>= 1;
            uint8_t pix;
            if (!mc) {
                if (!((d >> (23 - k)) & 1))
                    continue;
                pix = scol;
            } else {
                int pr = (int)((d >> (22 - (k & ~1))) & 3);
                if (pr == 0)
                    continue;
                pix = pr == 1 ? mm0 : pr == 2 ? scol : mm1;
            }
            s_mask[p] |= bit;
            s_col[p] = pix;
            s_prio[p] = prio;
        }
    }
}

static void render_seg(int p0, int p1, int pl, int pr)
{
    if (p0 == pl) {
        /* left border compare: vertical border checks, then main border */
        int rsel = (V.regs[0x11] >> 3) & 1;
        if (V.line == (rsel ? 0xFB : 0xF7))
            V.vborder = 1;
        if (V.line == (rsel ? 0x33 : 0x37) && (V.regs[0x11] & 0x10))
            V.vborder = 0;
        if (!V.vborder)
            V.mborder = 0;
    }
    render_graphics(p0, p1);
    render_sprites(p0, p1);

    /* collisions */
    uint8_t ss = 0, sb = 0;
    if (V.disp) {
        for (int p = p0; p < p1; p++) {
            uint8_t m = s_mask[p];
            if (!m)
                continue;
            if (m & (m - 1))
                ss |= m;
            if (g_fg[p])
                sb |= m;
        }
    }
    if (ss) {
        if (!V.coll_ss)
            vic_raise(0x04, C.clk);
        V.coll_ss |= ss;
    }
    if (sb) {
        if (!V.coll_sb)
            vic_raise(0x02, C.clk);
        V.coll_sb |= sb;
    }

    /* borders and output */
    uint8_t ec = V.regs[0x20] & 15;
    int row = FB_ROW(V.line);
    uint8_t *out = (row >= 0 && row < FB_H) ? C.fb[row] : NULL;
    for (int p = p0; p < p1; p++) {
        if (p == pr)
            V.mborder = 1;
        if (!out || p < FB_P0 || p >= FB_P0 + FB_W)
            continue;
        uint8_t pix;
        if (V.mborder)
            pix = ec;
        else if (s_mask[p] && !(s_prio[p] && g_fg[p]))
            pix = s_col[p];
        else
            pix = g_col[p];
        out[p - FB_P0] = pix;
    }
}

static void render_to(int p1)
{
    if (p1 > LINE_PIXELS)
        p1 = LINE_PIXELS;
    int p0 = V.render_p;
    if (p0 >= p1)
        return;
    int csel = (V.regs[0x16] >> 3) & 1;
    int pl = csel ? P_DISPLAY : P_DISPLAY + 7;         /* X 24 or 31 */
    int pr = csel ? P_DISPLAY + 320 : P_DISPLAY + 311; /* X 344 or 335 */
    if (p0 < pl && p1 > pl) {
        render_seg(p0, pl, pl, pr);
        render_seg(pl, p1, pl, pr);
    } else {
        render_seg(p0, p1, pl, pr);
    }
    V.render_p = (int16_t)p1;
}

/* pixel position reached at clock t on the current line */
static int pixel_at(uint64_t t)
{
    int64_t c = (int64_t)(t - V.line_t0); /* 0 = cycle 1 */
    if (c < 0)
        return 0;
    int64_t p = c * 8 + 4;
    return p > LINE_PIXELS ? LINE_PIXELS : (int)p;
}

/* ---- line sequencing ---------------------------------------------------- */

static uint8_t sprite_stolen(uint8_t fetching)
{
    /* BA low from 3 cycles before the first access of each sprite until
     * its last access; count the union over the fetch window. */
    uint32_t ba = 0;
    for (int s = 0; s < 8; s++)
        if (fetching & (1 << s))
            ba |= 0x1Fu << (2 * s);
    int n = 0;
    while (ba) {
        n += (int)(ba & 1);
        ba >>= 1;
    }
    return (uint8_t)n;
}

/* Logic of the end of line V.line (cycles 15/16 handled late, 55-63) and
 * the start of the next line. Called when the clock reaches the next line. */
static void next_line(void)
{
    /* a bad line stall of this line that was not applied yet (only when
     * the line boundary is crossed inside an instruction) is carried */
    if (V.bl_stall) {
        cfetch();
        V.bl_stall = 0;
        V.spr_stall = (uint16_t)(V.spr_stall + 43);
    }
    /* finish the old line */
    render_to(LINE_PIXELS);

    uint16_t line = V.line;
    uint8_t d015 = V.regs[0x15], d017 = V.regs[0x17];

    /* sprites: cycle 15/16 logic of this line (applied after it was
     * displayed, see the comment in the model description above) */
    for (int s = 0; s < 8; s++) {
        uint8_t bit = (uint8_t)(1 << s);
        if (!(V.dma & bit))
            continue;
        if (V.yexp_ff & bit)
            V.mcbase[s] = (uint8_t)((V.mcbase[s] + 3) & 63);
        if (V.mcbase[s] == 63) {
            V.dma &= (uint8_t)~bit;
            V.disp &= (uint8_t)~bit;
        }
    }
    /* cycle 55/56: expansion flip flop and DMA switch on */
    for (int s = 0; s < 8; s++) {
        uint8_t bit = (uint8_t)(1 << s);
        if (d017 & bit)
            V.yexp_ff ^= bit;
        else
            V.yexp_ff |= bit;
        if ((d015 & bit) && V.regs[2 * s + 1] == (line & 0xFF) && !(V.dma & bit)) {
            V.dma |= bit;
            V.mcbase[s] = 0;
            if (d017 & bit)
                V.yexp_ff &= (uint8_t)~bit;
        }
    }
    /* cycle 58: video counter and row counter */
    if (V.display_state)
        V.vc = (uint16_t)((V.vc + 40) & 0x3FF);
    if (V.rc == 7) {
        V.vcbase = V.vc;
        if (!badline_cond(line))
            V.display_state = 0;
    }
    if (V.display_state)
        V.rc = (uint8_t)((V.rc + 1) & 7);
    /* cycle 58: sprite MC load, display switch on, s-accesses */
    uint16_t base = vic_bank_base();
    uint16_t vm = (uint16_t)((V.regs[0x18] & 0xF0) << 6);
    uint8_t fetching = 0;
    for (int s = 0; s < 8; s++) {
        uint8_t bit = (uint8_t)(1 << s);
        V.mc[s] = V.mcbase[s];
        if ((V.dma & bit) && V.regs[2 * s + 1] == (line & 0xFF))
            V.disp |= bit;
        if (V.dma & bit) {
            uint8_t ptr = vic_mem(base, (uint16_t)(vm | 0x3F8 | s));
            uint16_t a = (uint16_t)(ptr << 6);
            for (int k = 0; k < 3; k++) {
                V.sdata[s][k] = vic_mem(base, (uint16_t)(a | V.mc[s]));
                V.mc[s] = (uint8_t)((V.mc[s] + 1) & 63);
            }
            fetching |= bit;
        }
    }
    /* cycle 63: vertical border */
    {
        int rsel = (V.regs[0x11] >> 3) & 1;
        if (line == (rsel ? 0xFB : 0xF7))
            V.vborder = 1;
        if (line == (rsel ? 0x33 : 0x37) && (V.regs[0x11] & 0x10))
            V.vborder = 0;
    }

    /* ---- new line ---- */
    V.line_t0 += VIC_CYCLES;
    line++;
    if (line >= VIC_LINES) {
        line = 0;
        V.frame_t0 = V.line_t0;
        V.den_frame = 0;
        V.vcbase = 0; /* reset outside the bad line range (line 0) */
        V.frame_done = 1;
        C.frame_count++;
    }
    V.line = line;
    V.render_p = 0;
    for (int s = 0; s < 8; s++)
        V.s_start[s] = -1;
    V.cfetched = 0;

    if (line == 0x30 && (V.regs[0x11] & 0x10))
        V.den_frame = 1;
    /* raster interrupt (line 0 compares one cycle later) */
    if (line == V.raster_cmp)
        vic_raise(0x01, V.line_t0 + (line == 0 ? 1 : 0));

    /* cycle 14: VC from VCBASE, bad line handling */
    V.vc = V.vcbase;
    V.badline = (uint8_t)badline_cond(line);
    if (V.badline) {
        V.rc = 0;
        V.display_state = 1;
    }

    /* stolen cycles: sprite DMA around the line start, bad line DMA at
     * cycles 12-54 */
    V.spr_stall = (uint16_t)(V.spr_stall + sprite_stolen(fetching));
    V.bl_stall = V.badline;
}

static void vic_schedule(void)
{
    uint64_t ne = V.line_t0 + VIC_CYCLES;
    if (V.spr_stall && V.line_t0 < ne)
        ne = V.line_t0;
    if (V.bl_stall && V.line_t0 + 11 < ne)
        ne = V.line_t0 + 11;
    V.next_event = ne;
}

/* Bring the VIC to clock t (process line starts) during an instruction.
 * Stalls stay pending and are applied by vic_event() afterwards. */
static void vic_sync(uint64_t t)
{
    while (t >= V.line_t0 + VIC_CYCLES)
        next_line();
}

/* Called between instructions when C.clk reached V.next_event. */
void vic_event(void)
{
    for (;;) {
        if (V.spr_stall && C.clk >= V.line_t0) {
            C.clk += V.spr_stall;
            V.spr_stall = 0;
            continue;
        }
        if (V.bl_stall && C.clk >= V.line_t0 + 11) {
            /* bad line: CPU halted from cycle 12 to cycle 54 */
            cfetch();
            V.bl_stall = 0;
            C.clk += 43;
            continue;
        }
        if (C.clk >= V.line_t0 + VIC_CYCLES) {
            next_line();
            continue;
        }
        break;
    }
    vic_schedule();
}

/* ---- registers -------------------------------------------------------- */

uint8_t vic_peek(uint16_t reg)
{
    reg &= 0x3F;
    switch (reg) {
    case 0x11:
        return (uint8_t)((V.regs[0x11] & 0x7F) | ((V.line & 0x100) >> 1));
    case 0x12:
        return (uint8_t)V.line;
    case 0x13: case 0x14:
        return 0;
    case 0x16:
        return V.regs[0x16] | 0xC0;
    case 0x18:
        return V.regs[0x18] | 0x01;
    case 0x19:
        return (uint8_t)(V.irr | 0x70 | ((V.irr & V.imr & 0x0F) ? 0x80 : 0));
    case 0x1A:
        return V.imr | 0xF0;
    case 0x1E:
        return V.coll_ss;
    case 0x1F:
        return V.coll_sb;
    default:
        if (reg >= 0x20 && reg <= 0x2E)
            return V.regs[reg] | 0xF0;
        if (reg >= 0x2F)
            return 0xFF;
        return V.regs[reg];
    }
}

uint8_t vic_read(uint16_t reg, uint64_t t)
{
    vic_sync(t);
    reg &= 0x3F;
    if (reg == 0x1E || reg == 0x1F) {
        render_to(pixel_at(t));
        uint8_t v;
        if (reg == 0x1E) {
            v = V.coll_ss;
            V.coll_ss = 0;
        } else {
            v = V.coll_sb;
            V.coll_sb = 0;
        }
        return v;
    }
    if (reg == 0x11 || reg == 0x12) {
        /* the raster counter changes at cycle 1 of a line */
        uint16_t line = V.line;
        if (reg == 0x11)
            return (uint8_t)((V.regs[0x11] & 0x7F) | ((line & 0x100) >> 1));
        return (uint8_t)line;
    }
    return vic_peek(reg);
}

void vic_write(uint16_t reg, uint8_t val, uint64_t t)
{
    vic_sync(t);
    reg &= 0x3F;
    if (reg >= 0x2F)
        return;
    /* everything before this cycle is drawn with the old value */
    render_to(pixel_at(t));
    switch (reg) {
    case 0x11: {
        V.regs[0x11] = val;
        uint16_t cmp = (uint16_t)((V.raster_cmp & 0xFF) | ((val & 0x80) << 1));
        if (cmp != V.raster_cmp) {
            V.raster_cmp = cmp;
            if (cmp == V.line)
                vic_raise(0x01, t);
        }
        if (V.line == 0x30 && (val & 0x10))
            V.den_frame = 1;
        /* a bad line condition can appear in the middle of a line */
        if (!V.badline && badline_cond(V.line) && t < V.line_t0 + 54) {
            V.badline = 1;
            V.display_state = 1;
            V.rc = 0;
            V.bl_stall = 1;
        }
        break;
    }
    case 0x12: {
        V.regs[0x12] = val;
        uint16_t cmp = (uint16_t)((V.raster_cmp & 0x100) | val);
        if (cmp != V.raster_cmp) {
            V.raster_cmp = cmp;
            if (cmp == V.line)
                vic_raise(0x01, t);
        }
        break;
    }
    case 0x17:
        V.regs[0x17] = val;
        V.yexp_ff |= (uint8_t)~val; /* flip flop set while MxYE is clear */
        break;
    case 0x19:
        V.irr &= (uint8_t)~(val & 0x0F);
        vic_update_irq(t);
        break;
    case 0x1A:
        V.imr = val & 0x0F;
        if ((V.irr & V.imr) && !(C.irq_lines & IRQ_SRC_VIC)) {
            irq_set(IRQ_SRC_VIC, 1);
            C.irq_since = t;
        } else {
            vic_update_irq(t);
        }
        break;
    case 0x1E: case 0x1F:
        break;
    default:
        V.regs[reg] = val;
        break;
    }
    vic_schedule();
}

void vic_reset(void)
{
    memset(&V, 0, sizeof V);
    V.line = 0;
    V.line_t0 = C.clk;
    V.frame_t0 = C.clk;
    for (int s = 0; s < 8; s++)
        V.s_start[s] = -1;
    V.yexp_ff = 0xFF;
    V.vborder = 1;
    V.mborder = 1;
    vic_schedule();
}
