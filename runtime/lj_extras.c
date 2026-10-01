/*
 * lj_extras.c - enhancements that need knowledge of the game code:
 * infinite lives and access to the score and the high score.
 *
 * Game facts (same in the DKS, Section 8 and CMM releases):
 *   $0360-$0362  score, BCD, low byte first (6 digits)
 *   $0363-$0365  high score, BCD, low byte first
 *   $0366        lives left (the screen shows this digit)
 *   $0FDF        DEC $0366: the only place where a life is taken
 *   $0823        STA $0363/$0364/$0365: high score cleared once at start
 *   $9D80-$9DB1  "if score > high score then copy score to high score"
 *
 * lj_extras_check() compares the loaded code with these bytes. When the
 * code is different (an unknown copy of the game), every function here does
 * nothing, so nothing is patched blindly.
 */
#include <string.h>
#include "c64.h"

#define A_SCORE 0x0360
#define A_HISCORE 0x0363
#define A_LIVES 0x0366
#define A_LOSE_LIFE 0x0FDF

static int extras_ok;

static const struct {
    uint16_t addr;
    uint8_t len;
    uint8_t bytes[20];
} expect[] = {
    /* DEC $0366 ; LDA #$0C ; STA $D413 */
    {0x0FDF, 8, {0xCE, 0x66, 0x03, 0xA9, 0x0C, 0x8D, 0x13, 0xD4}},
    /* STA $0363 ; STA $0364 ; STA $0365 */
    {0x0823, 9, {0x8D, 0x63, 0x03, 0x8D, 0x64, 0x03, 0x8D, 0x65, 0x03}},
    /* LDA $0365 ; SEC ; CMP $0362 ; BCC ; BNE */
    {0x9D80, 11, {0xAD, 0x65, 0x03, 0x38, 0xCD, 0x62, 0x03, 0x90, 0x16, 0xD0, 0x26}},
    /* copy score to high score ; RTS */
    {0x9D9F, 19, {0xAD, 0x62, 0x03, 0x8D, 0x65, 0x03, 0xAD, 0x61, 0x03, 0x8D, 0x64, 0x03,
                  0xAD, 0x60, 0x03, 0x8D, 0x63, 0x03, 0x60}},
};

void lj_extras_check(void)
{
    extras_ok = 1;
    for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++)
        if (memcmp(&C.ram[expect[i].addr], expect[i].bytes, expect[i].len) != 0)
            extras_ok = 0;
}

void lj_extras_disable(void) { extras_ok = 0; }

int lj_extras_available(void) { return extras_ok; }

void lj_poke(uint16_t addr, uint8_t v)
{
    if (addr < 2)
        return; /* the CPU port, not RAM */
    if (C.code_byte[addr])
        mem_wr_code(addr, v); /* keeps the recompiled code correct */
    else
        C.ram[addr] = v;
}

uint8_t lj_peek(uint16_t addr) { return C.ram[addr]; }

/* DEC $0366 (CE 66 03) becomes LDA $0366 (AD 66 03): same length, and the
 * next instruction (LDA #$0C) sets A and the flags again. */
void lj_cheat_infinite_lives(int on)
{
    if (!extras_ok || C.ram[A_LOSE_LIFE + 1] != 0x66 || C.ram[A_LOSE_LIFE + 2] != 0x03)
        return;
    uint8_t want = on ? 0xAD : 0xCE;
    uint8_t now = C.ram[A_LOSE_LIFE];
    if (now != want && (now == 0xAD || now == 0xCE))
        lj_poke(A_LOSE_LIFE, want);
}

static uint32_t bcd3(uint16_t a)
{
    uint32_t v = 0;
    for (int i = 2; i >= 0; i--) {
        uint8_t b = C.ram[a + i];
        uint32_t hi = b >> 4, lo = b & 15;
        if (hi > 9 || lo > 9)
            return 0;
        v = v * 100 + hi * 10 + lo;
    }
    return v;
}

uint32_t lj_score(void) { return extras_ok ? bcd3(A_SCORE) : 0; }

uint32_t lj_hiscore(void) { return extras_ok ? bcd3(A_HISCORE) : 0; }

int lj_lives(void) { return extras_ok ? C.ram[A_LIVES] : -1; }

void lj_set_hiscore(uint32_t v)
{
    if (!extras_ok)
        return;
    if (v > 999999)
        v = 999999;
    for (int i = 0; i < 3; i++) {
        uint32_t two = v % 100;
        lj_poke((uint16_t)(A_HISCORE + i), (uint8_t)(((two / 10) << 4) | (two % 10)));
        v /= 100;
    }
}
