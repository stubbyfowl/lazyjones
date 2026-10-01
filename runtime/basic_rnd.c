/*
 * basic_rnd.c - BASIC V2 RND function ($E097) for programs that call it
 * directly (Lazy Jones does: JSR $E097, then reads the seed byte at $8D).
 *
 * The result must match a real C64 bit for bit, because the game derives
 * all its random decisions from the seed. This file reproduces the
 * arithmetic of the Microsoft BASIC floating point routines involved,
 * including their carry-flag dependent quirks (the well known extra shift
 * in the multiplication when a multiplier byte is zero), operating on the
 * same zero page registers:
 *
 *   $22/$23 INDEX   $26-$29 RESHO (product)   $56 OLDOV   $61 FACEXP
 *   $62-$65 FAC mantissa   $66 FACSGN   $68 BITS (shift fill byte)
 *   $69 ARGEXP   $6A-$6D ARG mantissa   $6E ARGSGN   $6F ARISGN
 *   $70 FACOV (extension / rounding byte)   $8B-$8F RNDX (seed)
 *
 * Verified against the original ROM in tests/rnd (see docs/TESTING.md).
 */
#include "cpu_ops.h"

static inline uint8_t z(uint8_t a) { return ZRD(a); }
static inline void zs(uint8_t a, uint8_t v) { ZWR(a, v); }

static unsigned cyc; /* approximate cycle count of the ROM code */
static int vflag;    /* 6502 V flag as left by the last ADC/SBC */

/* five byte packed float constants used by RND */
static const uint8_t RMULC[5] = {0x98, 0x35, 0x44, 0x7A, 0x00}; /* 11879546 */
static const uint8_t RADDC[5] = {0x68, 0x28, 0xB1, 0x46, 0x00}; /* 3.927677739E-8 */

static void fac_zero_all(void) /* $B8F7 */
{
    zs(0x61, 0);
    zs(0x66, 0);
}

/* $BBA2 MOVFM: FAC = packed float at addr */
static void movfm(const uint8_t *m)
{
    zs(0x65, m[4]);
    zs(0x64, m[3]);
    zs(0x63, m[2]);
    zs(0x66, m[1]);
    zs(0x62, (uint8_t)(m[1] | 0x80));
    zs(0x61, m[0]);
    zs(0x70, 0);
    cyc += 50;
}

/* $BA8C CONUPK: ARG = packed float, ARISGN = sign(ARG) ^ sign(FAC) */
static void conupk(const uint8_t *m)
{
    zs(0x6D, m[4]);
    zs(0x6C, m[3]);
    zs(0x6B, m[2]);
    zs(0x6E, m[1]);
    zs(0x6F, (uint8_t)(m[1] ^ z(0x66)));
    zs(0x6A, (uint8_t)(m[1] | 0x80));
    zs(0x69, m[0]);
    cyc += 55;
}

/* $B936: if the mantissa overflowed (carry), shift right, bump exponent */
static void norm_carry(int c)
{
    if (!c)
        return;
    uint8_t e = (uint8_t)(z(0x61) + 1);
    zs(0x61, e);
    if (e == 0)
        lj_logf("BASIC FP: ?OVERFLOW ERROR in RND (ignored)");
    unsigned carry = 1;
    for (uint8_t a = 0x62; a <= 0x65; a++) {
        uint8_t v = z(a);
        zs(a, (uint8_t)((carry << 7) | (v >> 1)));
        carry = v & 1;
    }
    uint8_t v = z(0x70);
    zs(0x70, (uint8_t)((carry << 7) | (v >> 1)));
    cyc += 30;
}

/* $B8D7 NORMAL: normalize FAC (mantissa and FACOV) */
static void normal(void)
{
    uint8_t a = 0;
    while (z(0x62) == 0) {
        zs(0x62, z(0x63));
        zs(0x63, z(0x64));
        zs(0x64, z(0x65));
        zs(0x65, z(0x70));
        zs(0x70, 0);
        a = (uint8_t)(a + 8);
        vflag = 0; /* ADC #$08 on 0..$18 never overflows */
        cyc += 40;
        if (a == 0x20) {
            fac_zero_all();
            return;
        }
    }
    while (!(z(0x62) & 0x80)) {
        a = (uint8_t)(a + 1);
        unsigned carry = z(0x70) >> 7;
        zs(0x70, (uint8_t)(z(0x70) << 1));
        for (uint8_t r = 0x65; r >= 0x62; r--) {
            uint8_t v = z(r);
            zs(r, (uint8_t)((v << 1) | carry));
            carry = v >> 7;
        }
        cyc += 30;
    }
    uint8_t e = z(0x61);
    uint8_t r = (uint8_t)(a - e); /* SEC ; SBC $61 */
    vflag = ((a ^ e) & (a ^ r) & 0x80) != 0;
    if (a >= e) {
        fac_zero_all();
        return;
    }
    /* EOR #$FF ; ADC #$01 (carry clear) */
    uint8_t ne = (uint8_t)(e - a);
    vflag = ne == 0x80;
    zs(0x61, ne);
    cyc += 20;
}

static void byte_shift(uint8_t base)
{
    zs(0x70, z((uint8_t)(base + 4)));
    zs((uint8_t)(base + 4), z((uint8_t)(base + 3)));
    zs((uint8_t)(base + 3), z((uint8_t)(base + 2)));
    zs((uint8_t)(base + 2), z((uint8_t)(base + 1)));
    zs((uint8_t)(base + 1), z(0x68));
    cyc += 35;
}

/* $B985/$B999 shift helpers on the 4 byte mantissa at base+1..base+4.
 * Returns the extension byte (register A) after the shift; *c is the 6502
 * carry flag on entry and on exit. 'count' is the (negative) shift count in
 * the A register on entry to $B999. With first_byte the routine is entered
 * at $B985 (one whole byte shift first), as MULSHF ($B983) does. */
static uint8_t shiftr(uint8_t base, uint8_t count, int *c, int first_byte)
{
    uint8_t a = count;
    if (first_byte)
        byte_shift(base);
    for (;;) {
        unsigned s = (unsigned)a + 8 + (unsigned)*c; /* ADC #$08 */
        *c = s > 0xFF;
        a = (uint8_t)s;
        if ((a & 0x80) || a == 0) {
            /* shift right one whole byte; BITS fills from the top */
            byte_shift(base);
            continue;
        }
        break;
    }
    /* SBC #$08 */
    int borrow = !*c;
    int r = (int)a - 8 - borrow;
    *c = r >= 0;
    uint8_t y = (uint8_t)r;
    a = z(0x70);
    if (*c) {
        *c = 0;
        return a;
    }
    /* $B9A6: shift right bit by bit, -y times (y is negative) */
    for (;;) {
        uint8_t hi = z((uint8_t)(base + 1));
        unsigned cin = hi >> 7; /* arithmetic shift keeps bit 7 */
        unsigned out = hi & 1;
        zs((uint8_t)(base + 1), (uint8_t)((cin << 7) | (hi >> 1)));
        unsigned carry = out;
        for (uint8_t k = 2; k <= 4; k++) {
            uint8_t v = z((uint8_t)(base + k));
            zs((uint8_t)(base + k), (uint8_t)((carry << 7) | (v >> 1)));
            carry = v & 1;
        }
        a = (uint8_t)((carry << 7) | (a >> 1));
        cyc += 30;
        y++;
        if (y == 0)
            break;
    }
    *c = 0;
    return a;
}

/* MLTPL1 ($BA5E): RESHO+FACOV = (RESHO + bits * ARG) shifted, 8 steps */
static void mltpl1(uint8_t m)
{
    unsigned c = m & 1;
    uint8_t a = (uint8_t)((m >> 1) | 0x80);
    for (;;) {
        uint8_t y = a;
        if (c) {
            unsigned s = (unsigned)z(0x29) + z(0x6D);
            zs(0x29, (uint8_t)s);
            s = (unsigned)z(0x28) + z(0x6C) + (s >> 8);
            zs(0x28, (uint8_t)s);
            s = (unsigned)z(0x27) + z(0x6B) + (s >> 8);
            zs(0x27, (uint8_t)s);
            s = (unsigned)z(0x26) + z(0x6A) + (s >> 8);
            zs(0x26, (uint8_t)s);
            c = s >> 8;
            cyc += 37;
        }
        for (uint8_t r = 0x26; r <= 0x29; r++) {
            uint8_t v = z(r);
            zs(r, (uint8_t)((c << 7) | (v >> 1)));
            c = v & 1;
        }
        uint8_t v = z(0x70);
        zs(0x70, (uint8_t)((c << 7) | (v >> 1)));
        cyc += 37;
        a = y;
        c = a & 1;
        a >>= 1;
        if (a == 0)
            break;
    }
}

/* $BA28 FMULT: FAC = FAC * constant */
static void fmult(const uint8_t *m)
{
    conupk(m);
    if (z(0x61) == 0)
        return;
    /* $BAB7 MULDIV */
    int c;
    uint8_t ae = z(0x69);
    if (ae == 0) {
        fac_zero_all();
        return;
    }
    unsigned s = (unsigned)ae + z(0x61);
    uint8_t a = (uint8_t)s;
    if (s <= 0xFF) {
        if (!(a & 0x80)) {
            fac_zero_all();
            return;
        }
        s = (unsigned)a + 0x80;
    } else {
        if (a & 0x80) {
            lj_logf("BASIC FP: ?OVERFLOW ERROR in RND (ignored)");
            return;
        }
        s = (unsigned)a + 0x80;
    }
    c = s > 0xFF;
    a = (uint8_t)s;
    zs(0x61, a);
    if (a == 0)
        zs(0x66, 0);
    else
        zs(0x66, z(0x6F));
    cyc += 30;
    zs(0x26, 0);
    zs(0x27, 0);
    zs(0x28, 0);
    zs(0x29, 0);
    static const uint8_t order[4] = {0x70, 0x65, 0x64, 0x63};
    for (int i = 0; i < 4; i++) {
        uint8_t mb = z(order[i]);
        if (mb) {
            mltpl1(mb);
            c = 1; /* MLTPL1 ends with the sentinel bit in carry */
        } else {
            /* MULSHF: byte shift of RESHO with the carry quirk */
            (void)shiftr(0x25, 0, &c, 1);
        }
    }
    mltpl1(z(0x62));
    /* $BB8F: FAC = RESHO, normalize */
    zs(0x62, z(0x26));
    zs(0x63, z(0x27));
    zs(0x64, z(0x28));
    zs(0x65, z(0x29));
    normal();
}

/* $B947 NEGFAC: two's complement of the mantissa and FACOV */
static void negfac(void)
{
    zs(0x66, (uint8_t)~z(0x66));
    for (uint8_t a = 0x62; a <= 0x65; a++)
        zs(a, (uint8_t)~z(a));
    zs(0x70, (uint8_t)~z(0x70));
    uint8_t v = (uint8_t)(z(0x70) + 1);
    zs(0x70, v);
    if (v) return;
    for (uint8_t a = 0x65; a >= 0x62; a--) {
        v = (uint8_t)(z(a) + 1);
        zs(a, v);
        if (v) return;
    }
}

/* $B867 FADD: FAC = constant + FAC */
static void fadd(const uint8_t *m)
{
    conupk(m);
    if (z(0x61) == 0) {
        /* $BBFC MOVFA */
        zs(0x66, z(0x6E));
        for (int i = 5; i >= 1; i--)
            zs((uint8_t)(0x60 + i), z((uint8_t)(0x68 + i)));
        zs(0x70, 0);
        return;
    }
    zs(0x56, z(0x70));
    uint8_t x = 0x69;
    uint8_t ae = z(0x69);
    if (ae == 0)
        return;
    int diff = (int)ae - (int)z(0x61);
    uint8_t a = (uint8_t)diff;
    int c;
    if (diff != 0) {
        if (diff < 0) {
            zs(0x70, 0); /* shift ARG */
        } else {
            zs(0x61, ae);
            zs(0x66, z(0x6E));
            a = (uint8_t)(~a + 1); /* EOR #$FF ; ADC #0 with carry set */
            zs(0x56, 0);
            x = 0x61; /* shift FAC */
        }
        /* $B897 CMP #$F9 */
        c = a >= 0xF9;
        if ((uint8_t)(a - 0xF9) & 0x80) {
            a = shiftr(x, a, &c, 0);
        } else {
            uint8_t y = a;
            uint8_t ext = z(0x70);
            /* LSR $01,X then ROLSHF */
            uint8_t hi = z((uint8_t)(x + 1));
            zs((uint8_t)(x + 1), (uint8_t)(hi >> 1));
            unsigned carry = hi & 1;
            for (;;) {
                for (uint8_t k = 2; k <= 4; k++) {
                    uint8_t v = z((uint8_t)(x + k));
                    zs((uint8_t)(x + k), (uint8_t)((carry << 7) | (v >> 1)));
                    carry = v & 1;
                }
                ext = (uint8_t)((carry << 7) | (ext >> 1));
                y++;
                if (y == 0)
                    break;
                uint8_t h = z((uint8_t)(x + 1));
                carry = h & 1;
                zs((uint8_t)(x + 1), (uint8_t)((h & 0x80) | (h >> 1)));
            }
            a = ext;
            c = 0;
            cyc += 40;
        }
    } else {
        c = 1; /* SBC with equal exponents leaves carry set */
    }
    /* $B8A3 */
    if (!(z(0x6F) & 0x80)) {
        /* $B8FE: same signs, add */
        unsigned s = (unsigned)a + z(0x56) + (unsigned)c;
        zs(0x70, (uint8_t)s);
        s = (unsigned)z(0x65) + z(0x6D) + (s >> 8);
        zs(0x65, (uint8_t)s);
        s = (unsigned)z(0x64) + z(0x6C) + (s >> 8);
        zs(0x64, (uint8_t)s);
        s = (unsigned)z(0x63) + z(0x6B) + (s >> 8);
        zs(0x63, (uint8_t)s);
        s = (unsigned)z(0x62) + z(0x6A) + (s >> 8);
        zs(0x62, (uint8_t)s);
        cyc += 50;
        norm_carry((int)(s >> 8));
        return;
    }
    /* $B8A7: different signs, subtract the shifted operand from the other */
    uint8_t yb = (x == 0x69) ? 0x61 : 0x69;
    int s = (int)(uint8_t)~a + (int)z(0x56) + 1; /* SEC ; EOR #$FF ; ADC $56 */
    int cc = s > 0xFF;
    zs(0x70, (uint8_t)s);
    for (int k = 4; k >= 1; k--) {
        int d = (int)z((uint8_t)(yb + k)) - (int)z((uint8_t)(x + k)) - (cc ? 0 : 1);
        cc = d >= 0;
        zs((uint8_t)(0x61 + k), (uint8_t)d);
    }
    if (!cc)
        negfac();
    normal();
    cyc += 60;
}

/* $BC1B ROUND + $BBD4 MOVMF: store FAC (rounded) into the seed */
static int round_store(void)
{
    int c = 0;
    if (z(0x61) != 0) {
        uint8_t ov = z(0x70);
        zs(0x70, (uint8_t)(ov << 1));
        c = ov >> 7;
        if (c) {
            /* $B96F: increment the mantissa */
            uint8_t a;
            int done = 0;
            for (a = 0x65; a >= 0x62; a--) {
                uint8_t v = (uint8_t)(z(a) + 1);
                zs(a, v);
                if (v) {
                    done = 1;
                    break;
                }
            }
            if (!done)
                norm_carry(1); /* $B938 */
        }
    }
    zs(0x22, 0x8B);
    zs(0x23, 0x00);
    zs(0x8F, z(0x65));
    zs(0x8E, z(0x64));
    zs(0x8D, z(0x63));
    zs(0x8C, (uint8_t)((z(0x66) | 0x7F) & z(0x62)));
    zs(0x8B, z(0x61));
    zs(0x70, 0);
    cyc += 70;
    return c;
}

void basic_rnd(void)
{
    cyc = 0;
    /* SIGN of FAC */
    int sign;
    if (z(0x61) == 0)
        sign = 0;
    else
        sign = (z(0x66) & 0x80) ? -1 : 1;
    cyc += 20;
    if (sign == 0) {
        /* RND(0): mantissa from CIA1 timer A and the TOD clock */
        C.cpu.x = 0x00;
        C.cpu.y = 0xDC;
        zs(0x22, 0x00);
        zs(0x23, 0xDC);
        zs(0x62, RD(0xDC04, 0));
        zs(0x64, RD(0xDC05, 0));
        zs(0x63, RD(0xDC08, 0));
        zs(0x65, RD(0xDC09, 0));
        cyc += 60;
    } else {
        if (sign > 0) {
            uint8_t seed[5];
            for (int i = 0; i < 5; i++)
                seed[i] = z((uint8_t)(0x8B + i));
            movfm(seed);
            fmult(RMULC);
            fadd(RADDC);
        }
        /* swap mantissa bytes */
        uint8_t t = z(0x65);
        zs(0x65, z(0x62));
        zs(0x62, t);
        t = z(0x63);
        zs(0x63, z(0x64));
        zs(0x64, t);
        cyc += 30;
    }
    zs(0x66, 0);
    zs(0x70, z(0x61));
    zs(0x61, 0x80);
    normal();
    int c = round_store();
    /* register results of the ROM routine */
    C.cpu.a = z(0x61);
    C.cpu.x = 0x8B;
    C.cpu.y = 0x00;
    SETNZ(C.cpu.a);
    C.cpu.fc = (uint8_t)c;
    C.cpu.fv = (uint8_t)vflag;
    C.clk += cyc;
}
