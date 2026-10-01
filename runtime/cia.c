/*
 * cia.c - MOS 6526 CIA emulation (timers, interrupt control, ports, TOD).
 *
 * CIA1 ($DC00) drives the IRQ line and connects the keyboard matrix and
 * both joystick ports. CIA2 ($DD00) drives NMI and selects the VIC bank.
 *
 * Timers are evaluated lazily: a running timer stores its counter value at
 * a base clock and the time of its next underflow is computed from it. A
 * timer with latch value L underflows every L+1 cycles.
 */
#include <string.h>
#include "cpu_ops.h"

#define TOD_TICK_CYCLES 19705 /* 50 Hz mains input on a PAL machine */

static void cia_irq_out(cia_t *c, int which, int on, uint64_t t)
{
    c->irq = (uint8_t)on;
    if (which == 1) {
        int was = (C.irq_lines & IRQ_SRC_CIA1) != 0;
        irq_set(IRQ_SRC_CIA1, on);
        if (on && !was)
            C.irq_since = t;
    } else {
        int was = C.nmi_line;
        nmi_set(on);
        if (on && !was)
            C.nmi_since = t;
    }
}

static void cia_flag(cia_t *c, int which, uint8_t bits, uint64_t t)
{
    c->icr_data |= bits;
    if ((c->icr_data & c->icr_mask & 0x1F) && !c->irq)
        cia_irq_out(c, which, 1, t);
}

/* counter value of a phi2 timer at clock t (t >= base) */
static uint16_t timer_value(const cia_timer_t *tm, uint64_t t)
{
    if (!tm->running || t <= tm->base)
        return tm->counter;
    uint64_t e = t - tm->base;
    if (e > tm->counter)
        e = tm->counter; /* not reached: underflows are processed first */
    return (uint16_t)(tm->counter - e);
}

/* clock of the next underflow of a running phi2 timer, or UINT64_MAX */
static uint64_t timer_underflow_at(const cia_timer_t *tm)
{
    if (!tm->running)
        return UINT64_MAX;
    return tm->base + (uint64_t)tm->counter + 1;
}

static int tb_counts_ta(const cia_t *c)
{
    return (c->crb & 1) && ((c->crb & 0x40) != 0);
}

static void timer_b_tick(cia_t *c, int which, uint64_t t)
{
    /* timer B in cascade mode counts timer A underflows */
    if (c->tb.counter == 0) {
        cia_flag(c, which, 0x02, t + 1);
        c->tb.counter = c->tb.latch;
        if (c->crb & 0x08)
            c->crb &= (uint8_t)~1; /* one shot */
    } else {
        c->tb.counter--;
    }
}

static uint8_t bcd_inc(uint8_t v)
{
    v++;
    if ((v & 0x0F) > 9)
        v = (uint8_t)((v & 0xF0) + 0x10);
    return v;
}

static void tod_tick(cia_t *c, int which, uint64_t t)
{
    if (c->tod_halted)
        return;
    c->tod_ticks++;
    unsigned need = (c->cra & 0x80) ? 5u : 6u;
    if (c->tod_ticks < need)
        return;
    c->tod_ticks = 0;
    c->tod[0] = (uint8_t)(c->tod[0] + 1);
    if (c->tod[0] > 9) {
        c->tod[0] = 0;
        c->tod[1] = bcd_inc(c->tod[1]);
        if (c->tod[1] >= 0x60) {
            c->tod[1] = 0;
            c->tod[2] = bcd_inc(c->tod[2]);
            if (c->tod[2] >= 0x60) {
                c->tod[2] = 0;
                uint8_t pm = c->tod[3] & 0x80;
                uint8_t h = (uint8_t)(c->tod[3] & 0x1F);
                h = bcd_inc(h);
                if (h == 0x12)
                    pm ^= 0x80;
                if (h == 0x13)
                    h = 0x01;
                c->tod[3] = (uint8_t)(h | pm);
            }
        }
    }
    if (memcmp(c->tod, c->alarm, 4) == 0)
        cia_flag(c, which, 0x04, t);
}

static void cia_schedule(cia_t *c)
{
    uint64_t ne = c->tod_next;
    uint64_t u = timer_underflow_at(&c->ta);
    if (u < ne)
        ne = u;
    u = timer_underflow_at(&c->tb);
    if (u < ne)
        ne = u;
    c->next_event = ne;
}

/* process everything that happens up to clock t */
void cia_event(cia_t *c, int which, uint64_t t)
{
    for (;;) {
        uint64_t ua = timer_underflow_at(&c->ta);
        uint64_t ub = timer_underflow_at(&c->tb);
        uint64_t ut = c->tod_next;
        uint64_t first = ua < ub ? ua : ub;
        if (ut < first)
            first = ut;
        if (first > t)
            break;
        if (first == ua) {
            cia_flag(c, which, 0x01, ua + 1);
            c->ta.counter = c->ta.latch;
            c->ta.base = ua;
            if (c->cra & 0x08) {
                c->cra &= (uint8_t)~1;
                c->ta.running = 0;
            }
            if (tb_counts_ta(c))
                timer_b_tick(c, which, ua);
        } else if (first == ub) {
            cia_flag(c, which, 0x02, ub + 1);
            c->tb.counter = c->tb.latch;
            c->tb.base = ub;
            if (c->crb & 0x08) {
                c->crb &= (uint8_t)~1;
                c->tb.running = 0;
            }
        } else {
            c->tod_next += TOD_TICK_CYCLES;
            tod_tick(c, which, ut);
        }
    }
    cia_schedule(c);
}

static void sync(cia_t *c, int which, uint64_t t)
{
    if (t >= c->next_event)
        cia_event(c, which, t);
}

/* ---- ports ------------------------------------------------------------ */

static void cia1_lines(uint8_t *pa_out, uint8_t *pb_out)
{
    cia_t *c = &C.cia1;
    uint8_t pa = (uint8_t)((c->pra | (uint8_t)~c->ddra) & (uint8_t)~(C.joy2 & 0x1F));
    uint8_t pb = (uint8_t)((c->prb | (uint8_t)~c->ddrb) & (uint8_t)~(C.joy1 & 0x1F));
    /* a pressed key connects column line PAc with row line PBr; a low on
     * either side pulls the other one low too */
    for (int pass = 0; pass < 2; pass++) {
        for (int col = 0; col < 8; col++) {
            uint8_t rows = C.kb_matrix[col];
            if (!rows)
                continue;
            if (!(pa & (1 << col)))
                pb &= (uint8_t)~rows;
            if ((uint8_t)~pb & rows)
                pa &= (uint8_t)~(1 << col);
        }
    }
    *pa_out = pa;
    *pb_out = pb;
}

uint8_t cia1_port_a_in(void)
{
    uint8_t pa, pb;
    cia1_lines(&pa, &pb);
    return pa;
}

uint8_t cia1_port_b_in(void)
{
    uint8_t pa, pb;
    cia1_lines(&pa, &pb);
    return pb;
}

static uint8_t port_read(cia_t *c, int which, int b)
{
    if (which == 1)
        return b ? cia1_port_b_in() : cia1_port_a_in();
    if (!b) {
        uint8_t out = (uint8_t)(c->pra | (uint8_t)~c->ddra);
        /* serial bus: nothing connected, lines are high unless the C64
         * pulls them (outputs are inverted) */
        uint8_t clk_in = (out & 0x10) ? 0 : 0x40;
        uint8_t data_in = (out & 0x20) ? 0 : 0x80;
        return (uint8_t)((out & 0x3F) | clk_in | data_in);
    }
    return (uint8_t)(c->prb | (uint8_t)~c->ddrb);
}

/* ---- registers -------------------------------------------------------- */

uint8_t cia_peek(cia_t *c, int which, uint8_t reg)
{
    uint64_t t = C.clk;
    switch (reg & 0x0F) {
    case 0x0: return port_read(c, which, 0);
    case 0x1: return port_read(c, which, 1);
    case 0x2: return c->ddra;
    case 0x3: return c->ddrb;
    case 0x4: return (uint8_t)timer_value(&c->ta, t);
    case 0x5: return (uint8_t)(timer_value(&c->ta, t) >> 8);
    case 0x6: return (uint8_t)timer_value(&c->tb, t);
    case 0x7: return (uint8_t)(timer_value(&c->tb, t) >> 8);
    case 0x8: return c->tod_latched ? c->tod_latch[0] : c->tod[0];
    case 0x9: return c->tod_latched ? c->tod_latch[1] : c->tod[1];
    case 0xA: return c->tod_latched ? c->tod_latch[2] : c->tod[2];
    case 0xB: return c->tod_latched ? c->tod_latch[3] : c->tod[3];
    case 0xC: return c->sdr;
    case 0xD: return (uint8_t)(c->icr_data | (c->irq ? 0x80 : 0));
    case 0xE: return c->cra;
    default: return c->crb;
    }
}

uint8_t cia_read(cia_t *c, int which, uint8_t reg, uint64_t t)
{
    sync(c, which, t);
    reg &= 0x0F;
    switch (reg) {
    case 0x4: return (uint8_t)timer_value(&c->ta, t);
    case 0x5: return (uint8_t)(timer_value(&c->ta, t) >> 8);
    case 0x6: return (uint8_t)timer_value(&c->tb, t);
    case 0x7: return (uint8_t)(timer_value(&c->tb, t) >> 8);
    case 0x8: {
        uint8_t v = c->tod_latched ? c->tod_latch[0] : c->tod[0];
        c->tod_latched = 0;
        return v;
    }
    case 0xB:
        if (!c->tod_latched) {
            memcpy(c->tod_latch, c->tod, 4);
            c->tod_latched = 1;
        }
        return c->tod_latch[3];
    case 0xD: {
        uint8_t v = (uint8_t)(c->icr_data | (c->irq ? 0x80 : 0));
        c->icr_data = 0;
        if (c->irq)
            cia_irq_out(c, which, 0, t);
        return v;
    }
    default:
        return cia_peek(c, which, reg);
    }
}

static void timer_freeze(cia_timer_t *tm, uint64_t t)
{
    tm->counter = timer_value(tm, t);
    tm->base = t;
}

void cia_write(cia_t *c, int which, uint8_t reg, uint8_t val, uint64_t t)
{
    sync(c, which, t);
    reg &= 0x0F;
    switch (reg) {
    case 0x0: c->pra = val; break;
    case 0x1: c->prb = val; break;
    case 0x2: c->ddra = val; break;
    case 0x3: c->ddrb = val; break;
    case 0x4:
        c->ta.latch = (uint16_t)((c->ta.latch & 0xFF00) | val);
        break;
    case 0x5:
        c->ta.latch = (uint16_t)((c->ta.latch & 0x00FF) | (val << 8));
        if (!(c->cra & 1)) {
            c->ta.counter = c->ta.latch;
            c->ta.base = t;
        }
        break;
    case 0x6:
        c->tb.latch = (uint16_t)((c->tb.latch & 0xFF00) | val);
        break;
    case 0x7:
        c->tb.latch = (uint16_t)((c->tb.latch & 0x00FF) | (val << 8));
        if (!(c->crb & 1)) {
            c->tb.counter = c->tb.latch;
            c->tb.base = t;
        }
        break;
    case 0x8: case 0x9: case 0xA: case 0xB: {
        int i = reg - 8;
        uint8_t v = val;
        if (i == 0) v &= 0x0F;
        if (i == 1 || i == 2) v &= 0x7F;
        if (i == 3) {
            v &= 0x9F;
            /* writing 12 to the hour register flips AM/PM */
            if ((v & 0x1F) == 0x12 && !(c->crb & 0x80))
                v ^= 0x80;
        }
        if (c->crb & 0x80) {
            c->alarm[i] = v;
        } else {
            c->tod[i] = v;
            if (i == 3)
                c->tod_halted = 1;
            if (i == 0) {
                c->tod_halted = 0;
                c->tod_ticks = 0;
            }
        }
        if (memcmp(c->tod, c->alarm, 4) == 0)
            cia_flag(c, which, 0x04, t);
        break;
    }
    case 0xC:
        c->sdr = val;
        break;
    case 0xD:
        if (val & 0x80)
            c->icr_mask |= val & 0x1F;
        else
            c->icr_mask &= (uint8_t)~(val & 0x1F);
        if ((c->icr_data & c->icr_mask & 0x1F) && !c->irq)
            cia_irq_out(c, which, 1, t);
        break;
    case 0xE: {
        int was = c->ta.running;
        timer_freeze(&c->ta, t);
        if (val & 0x10)
            c->ta.counter = c->ta.latch; /* force load */
        c->cra = val & (uint8_t)~0x10;
        c->ta.running = (val & 1) && !(val & 0x20);
        if (c->ta.running && !was)
            c->ta.base = t + 1; /* start delay */
        break;
    }
    default: {
        int was = c->tb.running;
        timer_freeze(&c->tb, t);
        if (val & 0x10)
            c->tb.counter = c->tb.latch;
        c->crb = val & (uint8_t)~0x10;
        c->tb.running = (val & 1) && !(val & 0x60);
        if (c->tb.running && !was)
            c->tb.base = t + 1;
        break;
    }
    }
    cia_schedule(c);
}

void cia_reset(cia_t *c)
{
    memset(c, 0, sizeof *c);
    c->ta.latch = c->ta.counter = 0xFFFF;
    c->tb.latch = c->tb.counter = 0xFFFF;
    c->ta.base = c->tb.base = C.clk;
    c->tod[3] = 0x01;
    c->tod_next = C.clk + TOD_TICK_CYCLES;
    cia_schedule(c);
}
