/*
 * kernal.c - replacement for the C64 KERNAL ROM.
 *
 * The original KERNAL is copyrighted, so the runtime ships its own. Only
 * the parts a game can observe are reproduced: the documented entry points
 * and vectors, the interrupt entry/exit code, the zero page and $0200-$03FF
 * variables, and the visible results of the screen editor, keyboard scan
 * and jiffy clock (including register and flag results).
 *
 * The ROM image consists of a few lines of 6502 code (written here) and
 * "trap" instructions: opcode $02 followed by a trap number. When the CPU
 * executes a trap, kernal_trap() runs the routine in C. Every unused ROM
 * byte is $02, so a jump to any routine that is not implemented ends in
 * trap 2, which logs the address and returns to the caller.
 */
#include <string.h>
#include "cpu_ops.h"

enum {
    T_IRQ = 1,        /* $EA31 body: UDTIM, cursor blink, cassette, SCNKEY */
    T_UNKNOWN = 2,    /* fill byte */
    T_CHROUT,
    T_GETIN,
    T_CHRIN,
    T_SCNKEY,
    T_UDTIM,
    T_RDTIM,
    T_SETTIM,
    T_PLOT,
    T_SCREEN,
    T_IOBASE,
    T_STOP,
    T_CLRSCR,
    T_HOME,
    T_NOP_CLC,        /* harmless I/O calls: return with carry clear */
    T_READST,
    T_SETLFS,
    T_SETNAM,
    T_MEMTOP,
    T_MEMBOT,
    T_BASIC_RET,      /* RTS from the program back to BASIC */
    T_SETMSG,
    T_SETPOS,         /* $E56C: set cursor pointers from TBLX/PNTR */
    T_PRINT_SCREEN,   /* $E716: print to screen */
    T_RND,            /* $E097: BASIC RND (see basic_rnd.c) */
};

void basic_rnd(void);

/* ---- ROM assembly helpers -------------------------------------------- */

static void rom_put(uint16_t addr, const uint8_t *b, size_t n)
{
    memcpy(&hle_kernal[addr - 0xE000], b, n);
}

static void rom_trap_rts(uint16_t addr, uint8_t id)
{
    uint8_t b[3] = {0x02, id, 0x60};
    rom_put(addr, b, 3);
}

static void rom_jmp(uint16_t addr, uint16_t target)
{
    uint8_t b[3] = {0x4C, (uint8_t)target, (uint8_t)(target >> 8)};
    rom_put(addr, b, 3);
}

static void rom_jmp_ind(uint16_t addr, uint16_t vec)
{
    uint8_t b[3] = {0x6C, (uint8_t)vec, (uint8_t)(vec >> 8)};
    rom_put(addr, b, 3);
}

/* Keyboard decode tables (key index = column * 8 + row -> PETSCII), at the
 * addresses the KERNAL uses, because the KEYTAB pointer ($F5/$F6) points
 * into them. Values < 5 mark the modifier keys (1 SHIFT, 2 C=, 4 CTRL);
 * 3 is RUN/STOP. */
static const uint8_t kb_normal[65] = {
    0x14, 0x0D, 0x1D, 0x88, 0x85, 0x86, 0x87, 0x11,
    0x33, 0x57, 0x41, 0x34, 0x5A, 0x53, 0x45, 0x01,
    0x35, 0x52, 0x44, 0x36, 0x43, 0x46, 0x54, 0x58,
    0x37, 0x59, 0x47, 0x38, 0x42, 0x48, 0x55, 0x56,
    0x39, 0x49, 0x4A, 0x30, 0x4D, 0x4B, 0x4F, 0x4E,
    0x2B, 0x50, 0x4C, 0x2D, 0x2E, 0x3A, 0x40, 0x2C,
    0x5C, 0x2A, 0x3B, 0x13, 0x01, 0x3D, 0x5E, 0x2F,
    0x31, 0x5F, 0x04, 0x32, 0x20, 0x02, 0x51, 0x03,
    0xFF,
};
static const uint8_t kb_shift[65] = {
    0x94, 0x8D, 0x9D, 0x8C, 0x89, 0x8A, 0x8B, 0x91,
    0x23, 0xD7, 0xC1, 0x24, 0xDA, 0xD3, 0xC5, 0x01,
    0x25, 0xD2, 0xC4, 0x26, 0xC3, 0xC6, 0xD4, 0xD8,
    0x27, 0xD9, 0xC7, 0x28, 0xC2, 0xC8, 0xD5, 0xD6,
    0x29, 0xC9, 0xCA, 0x30, 0xCD, 0xCB, 0xCF, 0xCE,
    0xDB, 0xD0, 0xCC, 0xDD, 0x3E, 0x5B, 0xBA, 0x3C,
    0xA9, 0xC0, 0x5D, 0x93, 0x01, 0x3D, 0xDE, 0x3F,
    0x21, 0x5F, 0x04, 0x22, 0xA0, 0x02, 0xD1, 0x83,
    0xFF,
};
static const uint8_t kb_cbm[65] = {
    0x94, 0x8D, 0x9D, 0x8C, 0x89, 0x8A, 0x8B, 0x91,
    0x96, 0xB3, 0xB0, 0x97, 0xAD, 0xAE, 0xB1, 0x01,
    0x98, 0xB2, 0xAC, 0x99, 0xBC, 0xBB, 0xA3, 0xBD,
    0x9A, 0xB7, 0xA5, 0x9B, 0xBF, 0xB4, 0xB8, 0xBE,
    0x29, 0xA2, 0xB5, 0x30, 0xA7, 0xA1, 0xB9, 0xAA,
    0xA6, 0xAF, 0xB6, 0xDC, 0x3E, 0x5B, 0xA4, 0x3C,
    0xA8, 0xDF, 0x5D, 0x93, 0x01, 0x3D, 0xDE, 0x3F,
    0x81, 0x5F, 0x04, 0x95, 0xA0, 0x02, 0xAB, 0x83,
    0xFF,
};
static const uint8_t kb_ctrl[65] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x1C, 0x17, 0x01, 0x9F, 0x1A, 0x13, 0x05, 0xFF,
    0x9C, 0x12, 0x04, 0x1E, 0x03, 0x06, 0x14, 0x18,
    0x1F, 0x19, 0x07, 0x9E, 0x02, 0x08, 0x15, 0x16,
    0x12, 0x09, 0x0A, 0x92, 0x0D, 0x0B, 0x0F, 0x0E,
    0xFF, 0x10, 0x0C, 0xFF, 0xFF, 0x1B, 0x00, 0xFF,
    0x1C, 0xFF, 0x1D, 0xFF, 0xFF, 0x1F, 0x1E, 0xFF,
    0x90, 0x06, 0xFF, 0x05, 0xFF, 0xFF, 0x11, 0xFF,
    0xFF,
};
#define KT_NORMAL 0xEB81
#define KT_SHIFT 0xEBC2
#define KT_CBM 0xEC03
#define KT_CTRL 0xEC78

/* low byte of the start address of each screen row */
static uint8_t row_lo(int row) { return (uint8_t)((row * 40) & 0xFF); }

void kernal_build_rom(void)
{
    memset(hle_kernal, 0x02, sizeof hle_kernal);
    memset(hle_basic, 0x02, sizeof hle_basic);
    memset(hle_chargen, 0x00, sizeof hle_chargen);

    rom_put(KT_NORMAL, kb_normal, 65);
    rom_put(KT_SHIFT, kb_shift, 65);
    rom_put(KT_CBM, kb_cbm, 65);
    rom_put(KT_CTRL, kb_ctrl, 65);

    /* hardware vectors */
    static const uint8_t vec[6] = {0x43, 0xFE, 0xE2, 0xFC, 0x48, 0xFF};
    rom_put(0xFFFA, vec, 6);

    /* IRQ/BRK entry: save registers, dispatch through $0314/$0316 */
    static const uint8_t irq_entry[] = {
        0x48, 0x8A, 0x48, 0x98, 0x48,   /* PHA TXA PHA TYA PHA */
        0xBA, 0xBD, 0x04, 0x01,         /* TSX ; LDA $0104,X */
        0x29, 0x10, 0xF0, 0x03,         /* AND #$10 ; BEQ +3 */
        0x6C, 0x16, 0x03,               /* JMP ($0316) */
        0x6C, 0x14, 0x03,               /* JMP ($0314) */
    };
    rom_put(0xFF48, irq_entry, sizeof irq_entry);

    /* default IRQ handler $EA31 and the common exit points */
    static const uint8_t irq_default[] = {0x02, T_IRQ, 0x4C, 0x7E, 0xEA};
    rom_put(0xEA31, irq_default, sizeof irq_default);
    static const uint8_t irq_exit[] = {
        0xAD, 0x0D, 0xDC,               /* $EA7E LDA $DC0D */
        0x68, 0xA8, 0x68, 0xAA, 0x68,   /* $EA81 PLA TAY PLA TAX PLA */
        0x40,                           /* RTI */
    };
    rom_put(0xEA7E, irq_exit, sizeof irq_exit);
    rom_trap_rts(0xEA87, T_SCNKEY);

    /* NMI: SEI ; JMP ($0318). Default handler acknowledges CIA2. */
    static const uint8_t nmi_entry[] = {0x78, 0x6C, 0x18, 0x03};
    rom_put(0xFE43, nmi_entry, sizeof nmi_entry);
    static const uint8_t nmi_default[] = {
        0x48, 0x8A, 0x48, 0x98, 0x48,   /* PHA TXA PHA TYA PHA */
        0xA9, 0x7F, 0x8D, 0x0D, 0xDD,   /* LDA #$7F ; STA $DD0D */
        0xAC, 0x0D, 0xDD,               /* LDY $DD0D */
        0x4C, 0xBC, 0xFE,               /* JMP $FEBC */
    };
    rom_put(0xFE47, nmi_default, sizeof nmi_default);
    static const uint8_t nmi_exit[] = {0x68, 0xA8, 0x68, 0xAA, 0x68, 0x40};
    rom_put(0xFEBC, nmi_exit, sizeof nmi_exit);
    /* BRK default ($0316 -> $FE66): treat like a warm start; log it */
    rom_trap_rts(0xFE66, T_UNKNOWN);

    /* screen editor entry points used by programs directly */
    rom_trap_rts(0xE544, T_CLRSCR);
    rom_trap_rts(0xE566, T_HOME);
    rom_trap_rts(0xE56C, T_SETPOS);
    rom_trap_rts(0xE716, T_PRINT_SCREEN);
    rom_trap_rts(0xE505, T_SCREEN);
    rom_trap_rts(0xE50A, T_PLOT);
    rom_trap_rts(0xE500, T_IOBASE);

    /* routines behind the vectors at $031A-$0333 */
    rom_trap_rts(0xF1CA, T_CHROUT);
    rom_trap_rts(0xF13E, T_GETIN);
    rom_trap_rts(0xF157, T_CHRIN);
    rom_trap_rts(0xF6ED, T_STOP);
    rom_trap_rts(0xF34A, T_NOP_CLC); /* OPEN */
    rom_trap_rts(0xF291, T_NOP_CLC); /* CLOSE */
    rom_trap_rts(0xF20E, T_NOP_CLC); /* CHKIN */
    rom_trap_rts(0xF250, T_NOP_CLC); /* CKOUT */
    rom_trap_rts(0xF333, T_NOP_CLC); /* CLRCHN */
    rom_trap_rts(0xF32F, T_NOP_CLC); /* CLALL */
    rom_trap_rts(0xF69B, T_UDTIM);
    rom_trap_rts(0xF6DD, T_RDTIM);
    rom_trap_rts(0xF6E4, T_SETTIM);

    /* the KERNAL jump table */
    rom_jmp(0xFF81, 0xFF5B);            /* CINT (not implemented: trap 2) */
    rom_jmp(0xFF84, 0xFDA3);            /* IOINIT */
    rom_jmp(0xFF87, 0xFD50);            /* RAMTAS */
    rom_jmp(0xFF8A, 0xFD15);            /* RESTOR */
    rom_jmp(0xFF8D, 0xFD1A);            /* VECTOR */
    rom_jmp(0xFF90, 0xFE18);            /* SETMSG */
    rom_trap_rts(0xFE18, T_SETMSG);
    rom_jmp(0xFF93, 0xEDB9);            /* SECOND */
    rom_jmp(0xFF96, 0xEDC7);            /* TKSA */
    rom_jmp(0xFF99, 0xFE25);            /* MEMTOP */
    rom_trap_rts(0xFE25, T_MEMTOP);
    rom_jmp(0xFF9C, 0xFE34);            /* MEMBOT */
    rom_trap_rts(0xFE34, T_MEMBOT);
    rom_jmp(0xFF9F, 0xEA87);            /* SCNKEY */
    rom_jmp(0xFFA2, 0xFE21);            /* SETTMO */
    rom_jmp(0xFFA5, 0xEE13);            /* ACPTR */
    rom_jmp(0xFFA8, 0xEDDD);            /* CIOUT */
    rom_jmp(0xFFAB, 0xEDEF);            /* UNTLK */
    rom_jmp(0xFFAE, 0xEDFE);            /* UNLSN */
    rom_jmp(0xFFB1, 0xED0C);            /* LISTEN */
    rom_jmp(0xFFB4, 0xED09);            /* TALK */
    rom_jmp(0xFFB7, 0xFE07);            /* READST */
    rom_trap_rts(0xFE07, T_READST);
    rom_jmp(0xFFBA, 0xFE00);            /* SETLFS */
    rom_trap_rts(0xFE00, T_SETLFS);
    rom_jmp(0xFFBD, 0xFDF9);            /* SETNAM */
    rom_trap_rts(0xFDF9, T_SETNAM);
    rom_jmp_ind(0xFFC0, 0x031A);        /* OPEN */
    rom_jmp_ind(0xFFC3, 0x031C);        /* CLOSE */
    rom_jmp_ind(0xFFC6, 0x031E);        /* CHKIN */
    rom_jmp_ind(0xFFC9, 0x0320);        /* CHKOUT */
    rom_jmp_ind(0xFFCC, 0x0322);        /* CLRCHN */
    rom_jmp_ind(0xFFCF, 0x0324);        /* CHRIN */
    rom_jmp_ind(0xFFD2, 0x0326);        /* CHROUT */
    rom_jmp(0xFFD5, 0xF49E);            /* LOAD */
    rom_jmp(0xFFD8, 0xF5DD);            /* SAVE */
    rom_jmp(0xFFDB, 0xF6E4);            /* SETTIM */
    rom_jmp(0xFFDE, 0xF6DD);            /* RDTIM */
    rom_jmp_ind(0xFFE1, 0x0328);        /* STOP */
    rom_jmp_ind(0xFFE4, 0x032A);        /* GETIN */
    rom_jmp_ind(0xFFE7, 0x032C);        /* CLALL */
    rom_jmp(0xFFEA, 0xF69B);            /* UDTIM */
    rom_jmp(0xFFED, 0xE505);            /* SCREEN */
    rom_jmp(0xFFF0, 0xE50A);            /* PLOT */
    rom_jmp(0xFFF3, 0xE500);            /* IOBASE */

    /* BASIC's RND function, called directly by some games */
    rom_trap_rts(0xE097, T_RND);

    /* return address of BASIC's SYS command */
    rom_trap_rts(0xE147, T_BASIC_RET);
}

/* ---- memory helpers (CPU view, with code tracking and I/O) ----------- */

static uint8_t kpeek(uint16_t a) { return RD(a, 0); }
static void kpoke(uint16_t a, uint8_t v) { WR(a, v, 0); }
static uint16_t kpeek16(uint16_t a) { return (uint16_t)(kpeek(a) | (kpeek((uint16_t)(a + 1)) << 8)); }

#define Z_PNT 0xD1
#define Z_PNTR 0xD3
#define Z_QTSW 0xD4
#define Z_LNMX 0xD5
#define Z_TBLX 0xD6
#define Z_DATA 0xD7
#define Z_INSRT 0xD8
#define Z_LDTB1 0xD9
#define Z_USER 0xF3
#define Z_RVS 0xC7
#define V_COLOR 0x0286
#define V_GDCOL 0x0287
#define V_HIBASE 0x0288

/* $E9F0: PNT = start of screen row x */
static void set_pnt_row(int x)
{
    kpoke(Z_PNT, row_lo(x));
    kpoke(Z_PNT + 1, (uint8_t)((kpeek((uint16_t)(Z_LDTB1 + x)) & 0x03) | kpeek(V_HIBASE)));
}

/* $EA24: USER (colour RAM pointer) from PNT */
static void set_user(void)
{
    kpoke(Z_USER, kpeek(Z_PNT));
    kpoke(Z_USER + 1, (uint8_t)((kpeek(Z_PNT + 1) & 0x03) | 0xD8));
}

/* $E56C: find the logical line of the cursor row, set PNT, LNMX, USER */
static void set_cursor_pointers(void)
{
    int x = kpeek(Z_TBLX);
    uint8_t a = kpeek(Z_PNTR);
    while (x >= 0 && !(kpeek((uint16_t)(Z_LDTB1 + x)) & 0x80)) {
        a = (uint8_t)(a + 40);
        kpoke(Z_PNTR, a);
        x--;
    }
    if (x < 0)
        x = 0;
    set_pnt_row(x);
    a = 39;
    x++;
    while (x <= 25 && !(kpeek((uint16_t)(Z_LDTB1 + x)) & 0x80)) {
        a = (uint8_t)(a + 40);
        x++;
    }
    kpoke(Z_LNMX, a);
    set_user();
}

/* $E9FF: clear screen row x (spaces, colour = COLOR) */
static void clear_row(int x)
{
    set_pnt_row(x);
    set_user();
    uint16_t pnt = kpeek16(Z_PNT), user = kpeek16(Z_USER);
    uint8_t col = kpeek(V_COLOR);
    for (int y = 39; y >= 0; y--) {
        kpoke((uint16_t)(user + y), col);
        kpoke((uint16_t)(pnt + y), 0x20);
    }
}

static void home(void)
{
    kpoke(Z_TBLX, 0);
    kpoke(Z_PNTR, 0);
    set_cursor_pointers();
}

/* $E544 */
static void clear_screen(void)
{
    uint8_t y = (uint8_t)(kpeek(V_HIBASE) | 0x80);
    uint8_t a = 0;
    for (int x = 0; x < 26; x++) {
        kpoke((uint16_t)(Z_LDTB1 + x), y);
        unsigned s = (unsigned)a + 40;
        a = (uint8_t)s;
        if (s > 0xFF)
            y++;
    }
    kpoke((uint16_t)(Z_LDTB1 + 26), 0xFF);
    for (int x = 24; x >= 0; x--)
        clear_row(x);
    home();
}

/* $E9C8: copy screen row (and colour) 'from' to row 'to' */
static void copy_row(int to, int from)
{
    uint16_t dst = (uint16_t)(row_lo(to) | (((kpeek((uint16_t)(Z_LDTB1 + to)) & 3) | kpeek(V_HIBASE)) << 8));
    uint16_t src = (uint16_t)(row_lo(from) | (((kpeek((uint16_t)(Z_LDTB1 + from)) & 3) | kpeek(V_HIBASE)) << 8));
    for (int y = 39; y >= 0; y--) {
        kpoke((uint16_t)(dst + y), kpeek((uint16_t)(src + y)));
        uint16_t cs = (uint16_t)(((src & 0x03FF) | 0xD800) + y);
        uint16_t cd = (uint16_t)(((dst & 0x03FF) | 0xD800) + y);
        kpoke(cd, kpeek(cs) & 0x0F);
    }
}

/* $E8EA: scroll the screen up one row */
static void scroll_up(void)
{
    for (;;) {
        kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) - 1));
        kpoke(0xC9, (uint8_t)(kpeek(0xC9) - 1));
        kpoke(0x02A5, (uint8_t)(kpeek(0x02A5) - 1));
        for (int x = 0; x < 24; x++)
            copy_row(x, x + 1);
        clear_row(24);
        for (int x = 0; x < 24; x++) {
            uint8_t v = (uint8_t)(kpeek((uint16_t)(Z_LDTB1 + x)) & 0x7F);
            if (kpeek((uint16_t)(Z_LDTB1 + x + 1)) & 0x80)
                v |= 0x80;
            kpoke((uint16_t)(Z_LDTB1 + x), v);
        }
        kpoke(Z_LDTB1 + 24, (uint8_t)(kpeek(Z_LDTB1 + 24) | 0x80));
        if (kpeek(Z_LDTB1) & 0x80)
            break;
    }
    kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) + 1));
    kpoke(0x02A5, (uint8_t)(kpeek(0x02A5) + 1));
    /* the KERNAL checks the CTRL key here (slow scroll) */
    kpoke(0xDC00, 0x7F);
    (void)kpeek(0xDC01);
    kpoke(0xDC00, 0x7F);
}

/* $E87C: move to the start of the next logical line, scrolling if needed */
static void next_line(void)
{
    kpoke(0xC9, (uint8_t)(kpeek(0xC9) >> 1));
    int x = kpeek(Z_TBLX);
    for (;;) {
        x++;
        if (x == 25) {
            scroll_up();
            x = kpeek(Z_TBLX);
            x++;
            if (x > 24) x = 24;
        }
        if (kpeek((uint16_t)(Z_LDTB1 + x)) & 0x80)
            break;
    }
    kpoke(Z_TBLX, (uint8_t)x);
    set_cursor_pointers();
}

/* $E965-ish: open an empty row below the cursor row for a line link.
 * Rows below are moved down, the last row is lost. */
static void insert_row_below(int row)
{
    for (int x = 24; x > row + 1; x--)
        copy_row(x, x - 1);
    for (int x = 24; x > row + 1; x--) {
        uint8_t v = (uint8_t)(kpeek((uint16_t)(Z_LDTB1 + x)) & 0x7F);
        if (kpeek((uint16_t)(Z_LDTB1 + x - 1)) & 0x80)
            v |= 0x80;
        kpoke((uint16_t)(Z_LDTB1 + x), v);
    }
    clear_row(row + 1);
}

/* $EA13: put character a with colour x at the cursor */
static void put_char(uint8_t a, uint8_t col)
{
    kpoke(0xCD, 2);
    set_user();
    uint8_t y = kpeek(Z_PNTR);
    kpoke((uint16_t)(kpeek16(Z_PNT) + y), a);
    kpoke((uint16_t)(kpeek16(Z_USER) + y), col);
}

/* $E8B3: when the cursor is in the last column of a physical row of a
 * two row logical line, the row number moves on */
static void check_inc_row(void)
{
    uint8_t p = kpeek(Z_PNTR);
    if ((p == 39 || p == 79) && kpeek(Z_TBLX) < 25)
        kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) + 1));
}

static int link_warned;

/* $E6B6: advance the cursor after a character was printed */
static void advance_cursor(void)
{
    check_inc_row();
    uint8_t pntr = (uint8_t)(kpeek(Z_PNTR) + 1);
    kpoke(Z_PNTR, pntr);
    uint8_t lnmx = kpeek(Z_LNMX);
    if (lnmx >= pntr)
        return;
    if (lnmx == 79) {
        /* end of an 80 column logical line: go to the next line */
        kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) - 1));
        next_line();
        kpoke(Z_PNTR, 0);
        return;
    }
    /* end of a 40 column line: link the next physical row */
    if (!link_warned) {
        link_warned = 1;
        lj_logf("KERNAL: screen line link at row %d", kpeek(Z_TBLX));
    }
    int x = kpeek(Z_TBLX);
    if (x >= 25) {
        scroll_up();
        kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) - 1));
        x = kpeek(Z_TBLX);
    } else if (kpeek((uint16_t)(Z_LDTB1 + x)) & 0x80) {
        insert_row_below(x - 1);
    }
    kpoke((uint16_t)(Z_LDTB1 + x), (uint8_t)(kpeek((uint16_t)(Z_LDTB1 + x)) & 0x7F));
    if (x + 1 <= 25)
        kpoke((uint16_t)(Z_LDTB1 + x + 1), (uint8_t)(kpeek((uint16_t)(Z_LDTB1 + x + 1)) | 0x80));
    kpoke(Z_LNMX, (uint8_t)(lnmx + 40));
    int st = x;
    while (st > 0 && !(kpeek((uint16_t)(Z_LDTB1 + st)) & 0x80))
        st--;
    set_pnt_row(st);
}

static int colour_code(uint8_t c)
{
    static const uint8_t codes[16] = {
        0x90, 0x05, 0x1C, 0x9F, 0x9C, 0x1E, 0x1F, 0x9E,
        0x81, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B,
    };
    for (int i = 0; i < 16; i++)
        if (codes[i] == c)
            return i;
    return -1;
}

static void print_screen_code(uint8_t sc)
{
    if (kpeek(Z_RVS))
        sc |= 0x80;
    uint8_t ins = kpeek(Z_INSRT);
    if (ins)
        kpoke(Z_INSRT, (uint8_t)(ins - 1));
    put_char(sc, kpeek(V_COLOR));
    advance_cursor();
}

static void quote_test(uint8_t c)
{
    if (c == 0x22)
        kpoke(Z_QTSW, (uint8_t)(kpeek(Z_QTSW) ^ 1));
}

static void screen_print_body(uint8_t c);

/* $E716: print character c to the screen; $E6A8 exit */
static void screen_print(uint8_t c)
{
    screen_print_body(c);
    if (kpeek(Z_INSRT))
        kpoke(Z_QTSW, (uint8_t)(kpeek(Z_QTSW) >> 1));
}

static void screen_print_body(uint8_t c)
{
    kpoke(Z_DATA, c);
    kpoke(0xD0, 0);
    if (c < 0x80) {
        if (c == 0x0D) {
            kpoke(Z_INSRT, 0);
            kpoke(Z_RVS, 0);
            kpoke(Z_QTSW, 0);
            kpoke(Z_PNTR, 0);
            next_line();
            return;
        }
        if (c >= 0x20) {
            uint8_t sc = c < 0x60 ? (uint8_t)(c & 0x3F) : (uint8_t)(c & 0xDF);
            quote_test(c);
            print_screen_code(sc);
            return;
        }
        if (kpeek(Z_INSRT) || (kpeek(Z_QTSW) && c != 0x14)) {
            print_screen_code((uint8_t)(c | 0x80));
            return;
        }
        switch (c) {
        case 0x12: kpoke(Z_RVS, 0x12); return;
        case 0x13: home(); return;
        case 0x1D: { /* cursor right */
            uint8_t y = kpeek(Z_PNTR);
            check_inc_row();
            kpoke(Z_PNTR, (uint8_t)(y + 1));
            if (y < kpeek(Z_LNMX))
                return;
            kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) - 1));
            next_line();
            kpoke(Z_PNTR, 0);
            return;
        }
        case 0x11: { /* cursor down */
            uint8_t y = (uint8_t)(kpeek(Z_PNTR) + 40);
            kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) + 1));
            if (y <= kpeek(Z_LNMX)) {
                kpoke(Z_PNTR, y);
                return;
            }
            kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) - 1));
            y = (uint8_t)(y - 40);
            while (y >= 40)
                y = (uint8_t)(y - 40);
            kpoke(Z_PNTR, y);
            next_line();
            return;
        }
        case 0x14: { /* delete */
            uint8_t y = kpeek(Z_PNTR);
            if (y == 0 && kpeek(Z_TBLX) == 0)
                return;
            if (y == 0) {
                /* to the end of the previous row */
                kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) - 1));
                kpoke(Z_PNTR, 39);
                set_cursor_pointers();
                y = kpeek(Z_PNTR);
                uint16_t pnt = kpeek16(Z_PNT), user = kpeek16(Z_USER);
                kpoke((uint16_t)(pnt + y), 0x20);
                kpoke((uint16_t)(user + y), kpeek(V_COLOR));
                return;
            }
            uint16_t pnt = kpeek16(Z_PNT), user = kpeek16(Z_USER);
            uint8_t lnmx = kpeek(Z_LNMX);
            if (y == 40)
                kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) - 1));
            for (uint8_t i = y; i <= lnmx; i++) {
                kpoke((uint16_t)(pnt + i - 1), kpeek((uint16_t)(pnt + i)));
                kpoke((uint16_t)(user + i - 1), kpeek((uint16_t)(user + i)));
            }
            kpoke((uint16_t)(pnt + lnmx), 0x20);
            kpoke((uint16_t)(user + lnmx), kpeek(V_COLOR));
            kpoke(Z_PNTR, (uint8_t)(y - 1));
            return;
        }
        case 0x0E: /* lower case character set */
            kpoke(0xD018, (uint8_t)(kpeek(0xD018) | 0x02));
            return;
        case 0x08:
            kpoke(0x0291, (uint8_t)(kpeek(0x0291) | 0x80));
            return;
        case 0x09:
            kpoke(0x0291, (uint8_t)(kpeek(0x0291) & 0x7F));
            return;
        default: {
            int col = colour_code(c);
            if (col >= 0)
                kpoke(V_COLOR, (uint8_t)col);
            return;
        }
        }
    }
    /* shifted characters */
    uint8_t a = (uint8_t)(c & 0x7F);
    if (a == 0x7F)
        a = 0x5E;
    if (a >= 0x20) {
        quote_test(c);
        print_screen_code((uint8_t)(a | 0x40));
        return;
    }
    if (c == 0x8D) {
        kpoke(Z_INSRT, 0);
        kpoke(Z_RVS, 0);
        kpoke(Z_QTSW, 0);
        kpoke(Z_PNTR, 0);
        next_line();
        return;
    }
    if (kpeek(Z_QTSW) || (kpeek(Z_INSRT) && c != 0x94)) {
        print_screen_code((uint8_t)(a | 0x40 | 0x80) & 0xFF);
        return;
    }
    switch (c) {
    case 0x92: kpoke(Z_RVS, 0); return;
    case 0x93: clear_screen(); return;
    case 0x9D: { /* cursor left */
        uint8_t y = kpeek(Z_PNTR);
        if (y == 0) {
            if (kpeek(Z_TBLX) == 0)
                return;
            kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) - 1));
            kpoke(Z_PNTR, 39);
            set_cursor_pointers();
            return;
        }
        if (y == 40)
            kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) - 1));
        kpoke(Z_PNTR, (uint8_t)(y - 1));
        return;
    }
    case 0x91: { /* cursor up */
        if (kpeek(Z_TBLX) == 0)
            return;
        kpoke(Z_TBLX, (uint8_t)(kpeek(Z_TBLX) - 1));
        uint8_t y = kpeek(Z_PNTR);
        if (y >= 40) {
            kpoke(Z_PNTR, (uint8_t)(y - 40));
            return;
        }
        set_cursor_pointers();
        return;
    }
    case 0x94: { /* insert */
        uint16_t pnt = kpeek16(Z_PNT), user = kpeek16(Z_USER);
        uint8_t lnmx = kpeek(Z_LNMX), y = kpeek(Z_PNTR);
        if (kpeek((uint16_t)(pnt + lnmx)) != 0x20)
            return; /* line full (link handling not reproduced) */
        for (int i = lnmx; i > y; i--) {
            kpoke((uint16_t)(pnt + i), kpeek((uint16_t)(pnt + i - 1)));
            kpoke((uint16_t)(user + i), kpeek((uint16_t)(user + i - 1)));
        }
        kpoke((uint16_t)(pnt + y), 0x20);
        kpoke((uint16_t)(user + y), kpeek(V_COLOR));
        kpoke(Z_INSRT, (uint8_t)(kpeek(Z_INSRT) + 1));
        return;
    }
    case 0x8E: /* upper case character set */
        kpoke(0xD018, (uint8_t)(kpeek(0xD018) & 0xFD));
        return;
    default: {
        int col = colour_code(c);
        if (col >= 0)
            kpoke(V_COLOR, (uint8_t)col);
        return;
    }
    }
}

/* ---- keyboard (SCNKEY, $EA87) ------------------------------------------ */

/* Scan the keyboard like the KERNAL: sets SHFLAG ($028D), SFDX ($CB),
 * LSTX ($C5), puts new characters into the buffer and leaves $DC00 = $7F.
 * Returns an estimate of the cycles the ROM routine takes. */
static unsigned scnkey(void)
{
    static const uint16_t tabs[4] = {KT_NORMAL, KT_SHIFT, KT_CBM, KT_CTRL};
    unsigned cycles = 40;
    kpoke(0x028D, 0);
    kpoke(0xCB, 0x40);
    kpoke(0xDC00, 0x00);
    uint8_t x = kpeek(0xDC01);
    if (x != 0xFF) {
        /* scan 8 columns, then one read with no column selected (key
         * index 64, only joystick 1 can pull a row low there) */
        kpoke(0xF5, (uint8_t)KT_NORMAL);
        kpoke(0xF6, (uint8_t)(KT_NORMAL >> 8));
        uint8_t colsel = 0xFE;
        uint8_t key = 0;
        kpoke(0xDC00, colsel);
        for (;;) {
            uint8_t rows = kpeek(0xDC01);
            int done = 0;
            for (int r = 0; r < 8; r++) {
                if (!(rows & (1 << r))) {
                    uint8_t code = kpeek((uint16_t)(KT_NORMAL + key));
                    if (code < 5 && code != 3)
                        kpoke(0x028D, (uint8_t)(kpeek(0x028D) | code));
                    else
                        kpoke(0xCB, key);
                }
                key++;
                if (key >= 0x41) {
                    done = 1;
                    break;
                }
            }
            cycles += 140;
            if (done)
                break;
            colsel = (uint8_t)((colsel << 1) | 1);
            kpoke(0xDC00, colsel);
        }
        /* $EB48: decode table from the shift pattern */
        uint8_t shfl = kpeek(0x028D);
        if (shfl == 3) {
            if (shfl == kpeek(0x028E)) {
                kpoke(0xDC00, 0x7F);
                return cycles;
            }
            if (!(kpeek(0x0291) & 0x80))
                kpoke(0xD018, (uint8_t)(kpeek(0xD018) ^ 0x02));
        } else {
            unsigned i = shfl >= 4 ? 3u : shfl;
            kpoke(0xF5, (uint8_t)tabs[i]);
            kpoke(0xF6, (uint8_t)(tabs[i] >> 8));
        }
        /* $EAE0 */
        uint8_t y = kpeek(0xCB);
        x = kpeek((uint16_t)(kpeek16(0xF5) + y));
        if (y != kpeek(0xC5)) {
            kpoke(0x028C, 0x10);
        } else {
            uint8_t a = (uint8_t)(x & 0x7F);
            uint8_t rpt = kpeek(0x028A);
            if (!(rpt & 0x80)) {
                if (rpt & 0x40)
                    goto out;
                if (a != 0x7F) {
                    if (a != 0x14 && a != 0x20 && a != 0x1D && a != 0x11)
                        goto out;
                    goto repeat;
                }
                goto store;
            }
        repeat:
            if (kpeek(0x028C) != 0) {
                uint8_t d = (uint8_t)(kpeek(0x028C) - 1);
                kpoke(0x028C, d);
                if (d != 0)
                    goto out;
            }
            {
                uint8_t k = (uint8_t)(kpeek(0x028B) - 1);
                kpoke(0x028B, k);
                if (k != 0)
                    goto out;
            }
            kpoke(0x028B, 4);
            if (kpeek(0xC6) != 0)
                goto out;
        }
    }
store: /* $EB26 */
    kpoke(0xC5, kpeek(0xCB));
    kpoke(0x028E, kpeek(0x028D));
    if (x != 0xFF) {
        uint8_t n = kpeek(0xC6);
        if (n < kpeek(0x0289)) {
            kpoke((uint16_t)(0x0277 + n), x);
            kpoke(0xC6, (uint8_t)(n + 1));
        }
    }
out:
    kpoke(0xDC00, 0x7F);
    return cycles;
}

/* $F69B */
static void udtim(void)
{
    uint8_t t2 = (uint8_t)(kpeek(0xA2) + 1);
    kpoke(0xA2, t2);
    if (t2 == 0) {
        uint8_t t1 = (uint8_t)(kpeek(0xA1) + 1);
        kpoke(0xA1, t1);
        if (t1 == 0)
            kpoke(0xA0, (uint8_t)(kpeek(0xA0) + 1));
    }
    uint32_t j = ((uint32_t)kpeek(0xA0) << 16) | ((uint32_t)kpeek(0xA1) << 8) | kpeek(0xA2);
    if (j >= 0x4F1A01) {
        kpoke(0xA0, 0);
        kpoke(0xA1, 0);
        kpoke(0xA2, 0);
    }
    uint8_t a = kpeek(0xDC01);
    if (a & 0x80) {
        kpoke(0x91, a);
        return;
    }
    kpoke(0xDC00, 0xBD);
    uint8_t x = kpeek(0xDC01);
    kpoke(0xDC00, a);
    if ((uint8_t)(x + 1) == 0)
        kpoke(0x91, a);
}

/* ---- traps ---------------------------------------------------------------- */

static void do_rts(void)
{
    uint8_t lo = cpu_pull();
    uint8_t hi = cpu_pull();
    C.cpu.pc = (uint16_t)((lo | (hi << 8)) + 1);
}

static int unknown_reported;
int lj_debug_flags;

int kernal_trap(uint8_t id)
{
    uint16_t at = (uint16_t)(C.cpu.pc - 2);
    switch (id) {
    case T_IRQ: {
        udtim();
        /* cursor blink */
        if (kpeek(0xCC) == 0) {
            uint8_t cd = (uint8_t)(kpeek(0xCD) - 1);
            kpoke(0xCD, cd);
            if (cd == 0) {
                kpoke(0xCD, 0x14);
                uint8_t y = kpeek(Z_PNTR);
                uint8_t cf = kpeek(0xCF);
                kpoke(0xCF, (uint8_t)(cf >> 1));
                uint8_t x = kpeek(V_GDCOL);
                uint8_t a = kpeek((uint16_t)(kpeek16(Z_PNT) + y));
                if (!(cf & 1)) {
                    kpoke(0xCF, (uint8_t)(kpeek(0xCF) + 1));
                    kpoke(0xCE, a);
                    set_user();
                    kpoke(V_GDCOL, kpeek((uint16_t)(kpeek16(Z_USER) + y)));
                    x = kpeek(V_COLOR);
                    a = kpeek(0xCE);
                }
                a ^= 0x80;
                kpoke((uint16_t)(kpeek16(Z_PNT) + y), a);
                kpoke((uint16_t)(kpeek16(Z_USER) + y), x);
            }
        }
        /* cassette motor */
        uint8_t p = kpeek(0x01);
        if (p & 0x10) {
            kpoke(0xC0, 0);
            kpoke(0x01, (uint8_t)(kpeek(0x01) | 0x20));
        } else if (kpeek(0xC0) == 0) {
            kpoke(0x01, (uint8_t)(kpeek(0x01) & 0x1F));
        }
        C.clk += 120 + scnkey();
        return 1;
    }
    case T_SCNKEY:
        C.clk += scnkey();
        return 1;
    case T_UDTIM:
        udtim();
        C.clk += 60;
        return 1;
    case T_CHROUT: {
        uint8_t c = C.cpu.a;
        if (lj_debug_flags & 1)
            lj_logf("CHROUT $%02X row %d col %d from $%04X", c, kpeek(Z_TBLX), kpeek(Z_PNTR),
                    (uint16_t)((C.ram[0x101 + C.cpu.sp] | (C.ram[0x102 + C.cpu.sp] << 8)) - 2));
        if (kpeek(0x9A) == 3) {
            screen_print(c);
            C.cpu.fi = 0;
        } else {
            lj_logf("KERNAL: CHROUT to device %d ignored", kpeek(0x9A));
        }
        C.cpu.fc = 0;
        SETNZ(c);
        C.clk += 200;
        cpu_irq_recheck_now();
        return 1;
    }
    case T_PRINT_SCREEN: {
        uint8_t c = C.cpu.a;
        screen_print(c);
        C.cpu.fi = 0;
        C.cpu.fc = 0;
        SETNZ(c);
        C.clk += 180;
        cpu_irq_recheck_now();
        return 1;
    }
    case T_GETIN: {
        if (kpeek(0x99) != 0) {
            C.cpu.a = 0;
            C.cpu.fc = 0;
            SETNZ(0);
            return 1;
        }
        uint8_t n = kpeek(0xC6);
        if (n == 0) {
            C.cpu.a = 0;
            SETNZ(0);
            C.cpu.fc = 0;
            C.clk += 20;
            return 1;
        }
        uint8_t ch = kpeek(0x0277);
        uint8_t x = 0;
        do {
            kpoke((uint16_t)(0x0277 + x), kpeek((uint16_t)(0x0278 + x)));
            x++;
        } while (x != n);
        kpoke(0xC6, (uint8_t)(n - 1));
        C.cpu.x = x;
        C.cpu.y = ch;
        C.cpu.a = ch;
        SETNZ(ch);
        C.cpu.fi = 0;
        C.cpu.fc = 0;
        C.clk += 60;
        cpu_irq_recheck_now();
        return 1;
    }
    case T_CHRIN:
        /* screen line input is not used by the game; behave like GETIN */
        lj_logf("KERNAL: CHRIN called at $%04X", at);
        C.cpu.a = 0x0D;
        C.cpu.fc = 0;
        SETNZ(0x0D);
        return 1;
    case T_STOP: {
        uint8_t a = kpeek(0x91);
        C.cpu.fc = a >= 0x7F;
        SETNZ(a - 0x7F);
        C.cpu.a = a;
        if (a == 0x7F) {
            /* RUN/STOP: CLRCHN, clear the keyboard buffer */
            kpoke(0x9A, 3);
            kpoke(0x99, 0);
            kpoke(0xC6, 0);
            C.cpu.a = 0;
            C.cpu.x = 3;
        }
        C.clk += 20;
        return 1;
    }
    case T_RDTIM:
        /* RDTIM falls through into SETTIM in the ROM (ends with CLI) */
        C.cpu.a = kpeek(0xA2);
        C.cpu.x = kpeek(0xA1);
        C.cpu.y = kpeek(0xA0);
        SETNZ(C.cpu.y);
        C.cpu.fi = 0;
        C.clk += 30;
        cpu_irq_recheck_delayed();
        return 1;
    case T_SETTIM:
        kpoke(0xA2, C.cpu.a);
        kpoke(0xA1, C.cpu.x);
        kpoke(0xA0, C.cpu.y);
        C.cpu.fi = 0;
        C.clk += 24;
        cpu_irq_recheck_delayed();
        return 1;
    case T_PLOT:
        if (C.cpu.fc) {
            C.cpu.x = kpeek(Z_TBLX);
            C.cpu.y = kpeek(Z_PNTR);
        } else {
            kpoke(Z_TBLX, C.cpu.x);
            kpoke(Z_PNTR, C.cpu.y);
            set_cursor_pointers();
        }
        C.clk += 80;
        return 1;
    case T_SCREEN:
        C.cpu.x = 40;
        C.cpu.y = 25;
        return 1;
    case T_IOBASE:
        C.cpu.x = 0x00;
        C.cpu.y = 0xDC;
        return 1;
    case T_CLRSCR:
        clear_screen();
        C.clk += 16000;
        return 1;
    case T_HOME:
        home();
        C.clk += 60;
        return 1;
    case T_SETPOS:
        set_cursor_pointers();
        C.clk += 60;
        return 1;
    case T_NOP_CLC:
        C.cpu.fc = 0;
        return 1;
    case T_READST:
        C.cpu.a = kpeek(0x90);
        SETNZ(C.cpu.a);
        return 1;
    case T_SETLFS:
        kpoke(0xB8, C.cpu.a);
        kpoke(0xBA, C.cpu.x);
        kpoke(0xB9, C.cpu.y);
        return 1;
    case T_SETNAM:
        kpoke(0xB7, C.cpu.a);
        kpoke(0xBB, C.cpu.x);
        kpoke(0xBC, C.cpu.y);
        return 1;
    case T_MEMTOP:
        if (C.cpu.fc) {
            C.cpu.x = kpeek(0x0283);
            C.cpu.y = kpeek(0x0284);
        } else {
            kpoke(0x0283, C.cpu.x);
            kpoke(0x0284, C.cpu.y);
        }
        return 1;
    case T_MEMBOT:
        if (C.cpu.fc) {
            C.cpu.x = kpeek(0x0281);
            C.cpu.y = kpeek(0x0282);
        } else {
            kpoke(0x0281, C.cpu.x);
            kpoke(0x0282, C.cpu.y);
        }
        return 1;
    case T_RND:
        basic_rnd();
        return 1;
    case T_SETMSG:
        kpoke(0x9D, C.cpu.a);
        return 1;
    case T_BASIC_RET:
        lj_logf("game returned to BASIC");
        C.cpu.jammed = 2; /* the front end restarts the game */
        C.cpu.pc = at;
        return 1;
    case T_UNKNOWN:
    default:
        if (unknown_reported < 50) {
            unknown_reported++;
            lj_logf("KERNAL: unimplemented ROM routine at $%04X (trap %d)", at, id);
        }
        do_rts();
        return 1;
    }
}

/* ---- machine state after power on, LOAD and RUN ---------------------- */

void kernal_boot_state(void)
{
    /* zero page and system area as left by the KERNAL and BASIC */
    C.ram[0x2B] = 0x01; C.ram[0x2C] = 0x08;   /* TXTTAB */
    C.ram[0x33] = 0x00; C.ram[0x34] = 0xA0;   /* FRETOP */
    C.ram[0x37] = 0x00; C.ram[0x38] = 0xA0;   /* MEMSIZ */
    C.ram[0x90] = 0x00;                        /* STATUS */
    C.ram[0x91] = 0xFF;                        /* STKEY */
    C.ram[0x99] = 0x00;                        /* input: keyboard */
    C.ram[0x9A] = 0x03;                        /* output: screen */
    C.ram[0x9D] = 0x00;                        /* program mode */
    C.ram[0xB2] = 0x3C; C.ram[0xB3] = 0x03;   /* TAPE1 */
    /* RND seed after BASIC cold start, and the floating point accumulator
     * as left by evaluating the SYS address (positive, exponent $8C) */
    C.ram[0x8B] = 0x80; C.ram[0x8C] = 0x4F; C.ram[0x8D] = 0xC7;
    C.ram[0x8E] = 0x52; C.ram[0x8F] = 0x58;
    C.ram[0x61] = 0x8C; C.ram[0x62] = 0x00; C.ram[0x63] = 0x00;
    C.ram[0x64] = 0x08; C.ram[0x65] = 0x0D; C.ram[0x66] = 0x00;
    C.ram[0xC5] = 0x40;                        /* LSTX: no key */
    C.ram[0xC6] = 0x00;                        /* NDX */
    C.ram[0xC7] = 0x00;                        /* RVS */
    C.ram[0xCB] = 0x40;                        /* SFDX */
    C.ram[0xCC] = 0x01;                        /* cursor blink off */
    C.ram[0xCD] = 0x14;
    C.ram[0xD3] = 0x00;                        /* PNTR */
    C.ram[0xD4] = 0x00;
    C.ram[0xD7] = 0x0D;
    C.ram[0xD8] = 0x00;
    C.ram[0xF5] = (uint8_t)KT_NORMAL; C.ram[0xF6] = (uint8_t)(KT_NORMAL >> 8);
    C.ram[0x0281] = 0x00; C.ram[0x0282] = 0x08; /* MEMSTR */
    C.ram[0x0283] = 0x00; C.ram[0x0284] = 0xA0; /* MEMSIZ */
    C.ram[0x0286] = 0x0E;                        /* COLOR: light blue */
    C.ram[0x0287] = 0x0E;
    C.ram[0x0288] = 0x04;                        /* HIBASE */
    C.ram[0x0289] = 0x0A;                        /* XMAX */
    C.ram[0x028A] = 0x00;                        /* RPTFLG */
    C.ram[0x028B] = 0x04;                        /* KOUNT */
    C.ram[0x028C] = 0x10;                        /* DELAY */
    C.ram[0x028F] = 0x48; C.ram[0x0290] = 0xEB; /* KEYLOG */
    C.ram[0x0291] = 0x00;                        /* MODE */
    C.ram[0x02A6] = 0x01;                        /* PAL */
    static const uint8_t vectors[] = {
        0x8B, 0xE3, 0x83, 0xA4, 0x7C, 0xA5, 0x1A, 0xA7, 0xE4, 0xA7, 0x86, 0xAE,
        0x00, 0x00, 0x00, 0x00,                 /* $030C-$030F SYS registers */
        0x4C, 0x48, 0xB2, 0x00,                 /* USR */
        0x31, 0xEA, 0x66, 0xFE, 0x47, 0xFE, 0x4A, 0xF3, 0x91, 0xF2, 0x0E, 0xF2,
        0x50, 0xF2, 0x33, 0xF3, 0x57, 0xF1, 0xCA, 0xF1, 0xED, 0xF6, 0x3E, 0xF1,
        0x2F, 0xF3, 0x66, 0xFE, 0xA5, 0xF4, 0xED, 0xF5,
    };
    memcpy(&C.ram[0x0300], vectors, sizeof vectors);

    /* screen: empty, cursor home, all rows start a logical line */
    for (int i = 0; i < 1000; i++)
        C.ram[0x0400 + i] = 0x20;
    for (int i = 0; i < 1000; i++)
        C.colorram[i] = 0x0E;
    {
        uint8_t y = 0x84, a = 0;
        for (int x = 0; x < 26; x++) {
            C.ram[Z_LDTB1 + x] = y;
            unsigned s = (unsigned)a + 40;
            a = (uint8_t)s;
            if (s > 0xFF)
                y++;
        }
    }
    C.ram[Z_TBLX] = 0;
    C.ram[Z_PNT] = 0x00; C.ram[Z_PNT + 1] = 0x04;
    C.ram[Z_LNMX] = 39;
    C.ram[Z_USER] = 0x00; C.ram[Z_USER + 1] = 0xD8;

    /* stack: return into BASIC's SYS command */
    C.cpu.sp = 0xF6;
    static const uint8_t stack[] = {0x46, 0xE1, 0xE9, 0xA7, 0xA7, 0x79, 0xA6, 0x9C, 0xE3};
    memcpy(&C.ram[0x01F7], stack, sizeof stack);

    /* VIC-II as set up by the KERNAL */
    static const uint8_t vic_init[0x2F] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0x1B, 0, 0, 0, 0, 0xC8, 0, 0x15, 0, 0, 0, 0, 0, 0, 0,
        0x0E, 0x06, 0x01, 0x02, 0x03, 0x04, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0C,
    };
    for (int r = 0; r < 0x2F; r++)
        if (r != 0x19 && r != 0x1A && r != 0x1E && r != 0x1F)
            vic_write((uint16_t)r, vic_init[r], C.clk);
    vic_write(0x1A, 0x00, C.clk);
    vic_write(0x19, 0x0F, C.clk);

    /* CIA1: keyboard ports, timer A 60 Hz system interrupt */
    cia_write(&C.cia1, 1, 0x02, 0xFF, C.clk);
    cia_write(&C.cia1, 1, 0x03, 0x00, C.clk);
    cia_write(&C.cia1, 1, 0x00, 0x7F, C.clk);
    cia_write(&C.cia1, 1, 0x0D, 0x7F, C.clk);
    cia_write(&C.cia1, 1, 0x04, 0x25, C.clk);
    cia_write(&C.cia1, 1, 0x05, 0x40, C.clk);
    cia_write(&C.cia1, 1, 0x0D, 0x81, C.clk);
    cia_write(&C.cia1, 1, 0x0E, 0x11, C.clk);
    cia_write(&C.cia1, 1, 0x0F, 0x08, C.clk);
    cia_write(&C.cia1, 1, 0x0E, 0x81, C.clk); /* 50 Hz TOD, running */
    /* CIA2: VIC bank 0, serial bus idle */
    cia_write(&C.cia2, 2, 0x02, 0x3F, C.clk);
    cia_write(&C.cia2, 2, 0x00, 0x97, C.clk);
    cia_write(&C.cia2, 2, 0x03, 0x00, C.clk);
    cia_write(&C.cia2, 2, 0x0D, 0x7F, C.clk);
    cia_write(&C.cia2, 2, 0x0E, 0x08, C.clk);
    cia_write(&C.cia2, 2, 0x0F, 0x08, C.clk);

    /* 6510 port */
    mem_wr_slow(0x0000, 0x2F, 0);
    mem_wr_slow(0x0001, 0x37, 0);
}
