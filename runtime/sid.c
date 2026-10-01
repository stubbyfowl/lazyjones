/*
 * sid.c - MOS 6581 SID emulation.
 *
 * The chip is clocked cycle by cycle (985248 Hz) whenever it is synced:
 * before each register access and at the end of each frame. Register
 * writes therefore take effect at their exact cycle, which matters for
 * fast gate toggles (Lazy Jones relies on them).
 *
 * Oscillators, noise LFSR, ring modulation, hard sync, the test bit and
 * the envelope generator (rate counter periods, exponential decay, the
 * ADSR delay bug, zero freeze) follow the behaviour documented by reSID
 * (Dag Lem). The filter is a two integrator state variable filter run at
 * the chip clock with the 6581 cutoff curve. Output samples are the
 * average of all chip cycles in the sample period (box filter), followed
 * by a DC blocking high pass like the C64's output capacitor.
 */
#include <math.h>
#include <string.h>
#include "cpu_ops.h"

#define S C.sid

enum { ENV_ATTACK = 0, ENV_DECAY_SUSTAIN = 1, ENV_RELEASE = 2 };

static const uint16_t rate_period[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251
};

/* 6581 cutoff frequency (Hz) for FC = 0..2047, piecewise linear */
static const int f0_points[][2] = {
    {0, 220}, {128, 230}, {256, 250}, {384, 300}, {512, 420}, {640, 780},
    {768, 1600}, {832, 2300}, {896, 3200}, {960, 4300}, {992, 5000},
    {1008, 5400}, {1016, 5700}, {1023, 6000}, {1024, 4600}, {1032, 4800},
    {1056, 5300}, {1088, 6000}, {1120, 6600}, {1152, 7200}, {1280, 9500},
    {1408, 12000}, {1536, 14500}, {1664, 16000}, {1792, 17100},
    {1920, 17700}, {2047, 18000},
};
static float f0_table[2048];

#define AUDIO_BUF 8192
static int16_t abuf[AUDIO_BUF];
static size_t abuf_w, abuf_r;

static float w0dt;      /* 2 pi f0 / clock */
static float inv_q;     /* 1 / Q */

static void update_filter_coeffs(void)
{
    unsigned fc = (unsigned)((S.regs[0x15] & 7) | (S.regs[0x16] << 3));
    float f0 = f0_table[fc];
    w0dt = (float)(2.0 * 3.14159265358979 * f0 / C64_CLOCK_HZ);
    unsigned res = S.regs[0x17] >> 4;
    inv_q = 1.0f / (0.707f + (float)res / 15.0f);
}

static void build_f0_table(void)
{
    int n = (int)(sizeof f0_points / sizeof f0_points[0]);
    for (int fc = 0; fc < 2048; fc++) {
        int i = 0;
        while (i < n - 2 && f0_points[i + 1][0] <= fc)
            i++;
        int x0 = f0_points[i][0], x1 = f0_points[i + 1][0];
        int y0 = f0_points[i][1], y1 = f0_points[i + 1][1];
        float f;
        if (x1 == x0)
            f = (float)y1;
        else
            f = (float)y0 + (float)(y1 - y0) * (float)(fc - x0) / (float)(x1 - x0);
        f0_table[fc] = f;
    }
}

static inline uint16_t noise_out(uint32_t sh)
{
    return (uint16_t)(((sh & 0x100000) >> 9) | ((sh & 0x040000) >> 8) |
                      ((sh & 0x004000) >> 5) | ((sh & 0x000800) >> 3) |
                      ((sh & 0x000200) >> 2) | ((sh & 0x000020) << 1) |
                      ((sh & 0x000004) << 3) | ((sh & 0x000001) << 4));
}

static inline uint16_t wave_out(int vi)
{
    sid_voice_t *v = &S.v[vi];
    uint8_t ctrl = S.regs[vi * 7 + 4];
    unsigned w = ctrl >> 4;
    if (!w)
        return 0;
    uint32_t acc = v->acc;
    uint16_t out = 0xFFF;
    if (w & 1) { /* triangle, optionally ring modulated */
        uint32_t msb = acc & 0x800000;
        if (ctrl & 0x04)
            msb ^= S.v[(vi + 2) % 3].acc & 0x800000;
        out &= (uint16_t)(((msb ? ~acc : acc) >> 11) & 0xFFF);
    }
    if (w & 2) /* sawtooth */
        out &= (uint16_t)(acc >> 12);
    if (w & 4) { /* pulse */
        unsigned pw = (unsigned)(S.regs[vi * 7 + 2] | ((S.regs[vi * 7 + 3] & 0x0F) << 8));
        if (!((ctrl & 0x08) || (acc >> 12) >= pw))
            out = 0;
    }
    if (w & 8) /* noise */
        out &= noise_out(v->noise);
    return out;
}

static inline void env_clock(sid_voice_t *v, int vi)
{
    if (++v->rate_cnt & 0x8000)
        v->rate_cnt = (uint16_t)((v->rate_cnt + 1) & 0x7FFF);
    if (v->rate_cnt != v->rate_period)
        return;
    v->rate_cnt = 0;
    if (v->env_state == ENV_ATTACK || ++v->exp_cnt == v->exp_period) {
        v->exp_cnt = 0;
        if (v->hold_zero)
            return;
        switch (v->env_state) {
        case ENV_ATTACK:
            v->env = (uint8_t)(v->env + 1);
            if (v->env == 0xFF) {
                v->env_state = ENV_DECAY_SUSTAIN;
                v->rate_period = rate_period[S.regs[vi * 7 + 5] & 0x0F];
            }
            break;
        case ENV_DECAY_SUSTAIN:
            if (v->env != (uint8_t)((S.regs[vi * 7 + 6] >> 4) * 0x11))
                v->env--;
            break;
        default:
            v->env = (uint8_t)(v->env - 1);
            break;
        }
        switch (v->env) {
        case 0xFF: v->exp_period = 1; break;
        case 0x5D: v->exp_period = 2; break;
        case 0x36: v->exp_period = 4; break;
        case 0x1A: v->exp_period = 8; break;
        case 0x0E: v->exp_period = 16; break;
        case 0x06: v->exp_period = 30; break;
        case 0x00:
            v->exp_period = 1;
            v->hold_zero = 1;
            break;
        default: break;
        }
    }
}

static inline void push_sample(float x)
{
    /* DC blocker: y = x - x1 + R * y1 */
    float y = x - S.dc_hp_x + 0.9995f * S.dc_hp_y;
    S.dc_hp_x = x;
    S.dc_hp_y = y;
    float s = y * (1.0f / 7.0f);
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    size_t next = (abuf_w + 1) % AUDIO_BUF;
    if (next == abuf_r)
        abuf_r = (abuf_r + 1) % AUDIO_BUF; /* overrun: drop oldest */
    abuf[abuf_w] = (int16_t)lrintf(s);
    abuf_w = next;
}

/* run the chip for n cycles */
static void sid_clock(uint64_t n)
{
    const float wave_zero = 0x380, voice_dc = 0x800 * 0xFF;
    const float mixer_dc = (float)((-0xFFF * 0xFF / 18) >> 7);
    uint8_t filt = S.regs[0x17] & 0x07;
    uint8_t mode = S.regs[0x18];
    float vol = (float)(mode & 0x0F);
    int v3off = (mode & 0x80) && !(filt & 0x04);
    float facc = S.facc, cnt = S.fcnt;

    for (uint64_t i = 0; i < n; i++) {
        /* oscillators */
        uint32_t msb_rise = 0;
        for (int vi = 0; vi < 3; vi++) {
            sid_voice_t *v = &S.v[vi];
            uint8_t ctrl = S.regs[vi * 7 + 4];
            if (ctrl & 0x08)
                continue;
            uint32_t freq = (uint32_t)(S.regs[vi * 7] | (S.regs[vi * 7 + 1] << 8));
            uint32_t prev = v->acc;
            v->acc = (prev + freq) & 0xFFFFFF;
            if (!(prev & 0x800000) && (v->acc & 0x800000))
                msb_rise |= 1u << vi;
            if (!(prev & 0x080000) && (v->acc & 0x080000)) {
                uint32_t sh = v->noise;
                uint32_t b = ((sh >> 22) ^ (sh >> 17)) & 1;
                v->noise = ((sh << 1) & 0x7FFFFF) | b;
            }
        }
        /* hard sync: a rising MSB of the source resets the destination */
        if (msb_rise) {
            for (int vi = 0; vi < 3; vi++) {
                int src = (vi + 2) % 3;
                uint8_t ctrl = S.regs[vi * 7 + 4];
                uint8_t sctrl = S.regs[src * 7 + 4];
                if ((ctrl & 0x02) && (msb_rise & (1u << src)) &&
                    !((sctrl & 0x02) && (msb_rise & (1u << ((src + 2) % 3)))))
                    S.v[vi].acc = 0;
            }
        }
        /* envelopes and voice outputs */
        float vo[3];
        for (int vi = 0; vi < 3; vi++) {
            sid_voice_t *v = &S.v[vi];
            env_clock(v, vi);
            uint16_t w = wave_out(vi);
            v->out12 = w;
            vo[vi] = (((float)w - wave_zero) * (float)v->env + voice_dc) * (1.0f / 128.0f);
        }
        if (v3off)
            vo[2] = 0;
        float vi_f = 0, vnf = 0;
        for (int k = 0; k < 3; k++) {
            if (filt & (1 << k))
                vi_f += vo[k];
            else
                vnf += vo[k];
        }
        /* state variable filter */
        float hp = S.bp * inv_q - S.lp - vi_f;
        S.bp -= w0dt * hp;
        S.lp -= w0dt * S.bp;
        float vf = 0;
        if (mode & 0x10) vf += S.lp;
        if (mode & 0x20) vf += S.bp;
        if (mode & 0x40) vf += hp;
        facc += (vnf + vf + mixer_dc) * vol;
        cnt += 1.0f;

        S.sample_frac += 1.0;
        if (S.sample_frac >= S.cycles_per_sample) {
            S.sample_frac -= S.cycles_per_sample;
            push_sample(facc / cnt);
            facc = 0;
            cnt = 0;
        }
    }
    S.facc = facc;
    S.fcnt = cnt;
}

void sid_sync(uint64_t t)
{
    if (t <= S.clk)
        return;
    uint64_t n = t - S.clk;
    S.clk = t;
    sid_clock(n);
}

static void write_ctrl(int vi, uint8_t val)
{
    sid_voice_t *v = &S.v[vi];
    uint8_t old = S.regs[vi * 7 + 4];
    /* test bit */
    if (val & 0x08) {
        v->acc = 0;
        v->noise = 0;
    } else if (old & 0x08) {
        v->noise = 0x7FFFF8;
    }
    uint8_t gate = val & 1;
    if (!v->gate && gate) {
        v->env_state = ENV_ATTACK;
        v->rate_period = rate_period[S.regs[vi * 7 + 5] >> 4];
        v->hold_zero = 0;
    } else if (v->gate && !gate) {
        v->env_state = ENV_RELEASE;
        v->rate_period = rate_period[S.regs[vi * 7 + 6] & 0x0F];
    }
    v->gate = gate;
}

void sid_write(uint8_t reg, uint8_t val, uint64_t t)
{
    sid_sync(t);
    reg &= 0x1F;
    S.bus_value = val;
    S.bus_clk = t;
    if (reg > 0x18)
        return;
    if (reg < 21) {
        int vi = reg / 7, r = reg % 7;
        sid_voice_t *v = &S.v[vi];
        if (r == 4) {
            write_ctrl(vi, val);
        } else if (r == 5) {
            if (v->env_state == ENV_ATTACK)
                v->rate_period = rate_period[val >> 4];
            else if (v->env_state == ENV_DECAY_SUSTAIN)
                v->rate_period = rate_period[val & 0x0F];
        } else if (r == 6) {
            if (v->env_state == ENV_RELEASE)
                v->rate_period = rate_period[val & 0x0F];
        }
    }
    S.regs[reg] = val;
    if (reg >= 0x15 && reg <= 0x17)
        update_filter_coeffs();
}

uint8_t sid_read(uint8_t reg, uint64_t t)
{
    sid_sync(t);
    reg &= 0x1F;
    switch (reg) {
    case 0x19: case 0x1A:
        return 0xFF; /* no paddles connected */
    case 0x1B:
        return (uint8_t)(wave_out(2) >> 4);
    case 0x1C:
        return S.v[2].env;
    default:
        /* write-only register: the data bus keeps the last written value
         * for a short while */
        return (t - S.bus_clk < 0x2000) ? S.bus_value : 0;
    }
}

void sid_post_load(void)
{
    build_f0_table();
    update_filter_coeffs();
}

void sid_set_sample_rate(double rate)
{
    if (rate < 8000.0)
        rate = 8000.0;
    S.cycles_per_sample = C64_CLOCK_HZ / rate;
}

void sid_reset(int sample_rate)
{
    memset(&S, 0, sizeof S);
    build_f0_table();
    for (int vi = 0; vi < 3; vi++) {
        sid_voice_t *v = &S.v[vi];
        v->noise = 0x7FFFF8;
        v->env_state = ENV_RELEASE;
        v->rate_period = rate_period[0];
        v->exp_period = 1;
        v->hold_zero = 1;
    }
    S.clk = C.clk;
    sid_set_sample_rate(sample_rate > 0 ? sample_rate : 48000);
    update_filter_coeffs();
    abuf_w = abuf_r = 0;
}

size_t sid_audio_available(void)
{
    return (abuf_w + AUDIO_BUF - abuf_r) % AUDIO_BUF;
}

size_t sid_audio_read(int16_t *out, size_t max)
{
    size_t n = 0;
    while (n < max && abuf_r != abuf_w) {
        out[n++] = abuf[abuf_r];
        abuf_r = (abuf_r + 1) % AUDIO_BUF;
    }
    return n;
}
