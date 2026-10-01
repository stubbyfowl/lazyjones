/*
 * fe.c - portable front end (see fe.h).
 *
 * Default controls (all buttons can be mapped in CONTROLS):
 *   D-pad / left stick ... joystick (port 2)
 *   A, B, X, Y ........... fire
 *   START ................ the game's pause key (P)
 *   R1, R2 ............... the game's music key (M)
 *   L1, L2 ............... fast forward (hold)
 *   SELECT ............... this menu (Back and Menu keys always open it)
 *
 * Enhancements: infinite lives, saved high score, fast forward, auto fire,
 * four save state slots with quick save and quick load.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fe.h"
#include "../runtime/lj.h"
#include "font8x8.h"

#define STATE_MAGIC "LJST0001"

static const uint32_t map_btn[FE_MAP_COUNT] = {
    FE_BTN_A, FE_BTN_B, FE_BTN_X, FE_BTN_Y, FE_BTN_L1, FE_BTN_R1,
    FE_BTN_L2, FE_BTN_R2, FE_BTN_START, FE_BTN_SELECT,
};
static const char *const map_name[FE_MAP_COUNT] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "START", "SELECT",
};
static const char *const map_key[FE_MAP_COUNT] = {
    "map_a", "map_b", "map_x", "map_y", "map_l1", "map_r1", "map_l2", "map_r2",
    "map_start", "map_select",
};
static const int map_default[FE_MAP_COUNT] = {
    FE_ACT_FIRE, FE_ACT_FIRE, FE_ACT_FIRE, FE_ACT_FIRE, FE_ACT_FAST, FE_ACT_MUSIC,
    FE_ACT_FAST, FE_ACT_MUSIC, FE_ACT_PAUSE, FE_ACT_MENU,
};
static const char *const act_name[FE_ACT_COUNT] = {
    "NOTHING", "FIRE", "PAUSE", "MUSIC ON/OFF", "MENU", "FAST FORWARD",
    "QUICK SAVE", "QUICK LOAD",
};

/* ---- menu structure ----------------------------------------------------- */

enum {
    I_RESUME, I_SAVE, I_LOAD, I_RESET, I_GO_VIDEO, I_GO_SOUND, I_GO_CONTROLS,
    I_GO_EXTRAS, I_QUIT,
    I_SCALE, I_FILTER, I_BORDER,
    I_SOUND, I_VOLUME,
    I_MAP0, I_MAP_LAST = I_MAP0 + FE_MAP_COUNT - 1, I_AUTOFIRE, I_DEFAULTS,
    I_LIVES, I_FFSPEED, I_AUTORESUME, I_HISCORE,
    I_BACK,
};

enum { P_MAIN, P_VIDEO, P_SOUND, P_CONTROLS, P_EXTRAS, P_COUNT };

#define MAX_ROWS 13
static const struct {
    const char *title;
    int go_item; /* the item on the main page that opens this page */
    int n;
    int items[MAX_ROWS];
} pages[P_COUNT] = {
    {"LAZY JONES", -1, 9, {I_RESUME, I_SAVE, I_LOAD, I_RESET, I_GO_VIDEO, I_GO_SOUND,
                           I_GO_CONTROLS, I_GO_EXTRAS, I_QUIT}},
    {"VIDEO", I_GO_VIDEO, 4, {I_SCALE, I_FILTER, I_BORDER, I_BACK}},
    {"SOUND", I_GO_SOUND, 3, {I_SOUND, I_VOLUME, I_BACK}},
    {"CONTROLS", I_GO_CONTROLS, 13, {I_MAP0, I_MAP0 + 1, I_MAP0 + 2, I_MAP0 + 3, I_MAP0 + 4,
                                     I_MAP0 + 5, I_MAP0 + 6, I_MAP0 + 7, I_MAP0 + 8, I_MAP0 + 9,
                                     I_AUTOFIRE, I_DEFAULTS, I_BACK}},
    {"ENHANCEMENTS", I_GO_EXTRAS, 5, {I_LIVES, I_FFSPEED, I_AUTORESUME, I_HISCORE, I_BACK}},
};

#define MENU_Y0 40
#define MENU_DY 12

static struct {
    char dir[512];
    int game_ok;
    int extras;              /* game hooks available (lj_extras_available) */
    fe_settings_t set;
    fe_input_t in;
    uint32_t prev_held;
    int menu_open;
    int page, sel;
    int confirm;             /* item that waits for a second A press, or -1 */
    int quit;
    uint32_t repeat_btn;
    int repeat_count;
    int stick_latch;
    char toast[48];
    int toast_frames;
    int key_p, key_m;        /* game keys held this frame */
    int fire_count;          /* frames fire is held (auto fire) */
    int fast;                /* fast forward this frame */
    uint32_t rgba[FE_W * FE_H];
    uint32_t pal[16];
    uint8_t *state_buf;
    size_t state_size;
    int rate;                /* output sample rate */
    /* high score */
    uint32_t best;           /* saved best score */
    uint32_t hi_prev;        /* game's high score at the last frame */
    int hi_pending;          /* it changed in a game without the cheat */
    int hi_force;            /* write 'best' into the game at the next chance */
    int cheated;             /* the current game used infinite lives */
} fe;

/* ---- files ----------------------------------------------------------- */

static void path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", fe.dir, name);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void controls_defaults(void)
{
    for (int k = 0; k < FE_MAP_COUNT; k++)
        fe.set.map[k] = map_default[k];
    fe.set.autofire = 0;
}

static void settings_defaults(void)
{
    fe.set.scale = FE_SCALE_FIT;
    fe.set.filter = FE_FILTER_SHARP;
    fe.set.border = FE_BORDER_FULL;
    fe.set.sound = 1;
    fe.set.volume = 8;
    fe.set.autoresume = 1;
    controls_defaults();
    fe.set.slot = 1;
    fe.set.inf_lives = 0;
    fe.set.ff_speed = 3;
}

static void settings_load(void)
{
    char p[600];
    path(p, sizeof p, "settings.txt");
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    char key[32];
    int v;
    while (fscanf(f, "%31s %d", key, &v) == 2) {
        if (!strcmp(key, "scale")) fe.set.scale = clampi(v, 0, FE_SCALE_COUNT - 1);
        else if (!strcmp(key, "filter")) fe.set.filter = clampi(v, 0, FE_FILTER_COUNT - 1);
        else if (!strcmp(key, "border")) fe.set.border = clampi(v, 0, FE_BORDER_COUNT - 1);
        else if (!strcmp(key, "sound")) fe.set.sound = v != 0;
        else if (!strcmp(key, "volume")) fe.set.volume = clampi(v, 1, 10);
        else if (!strcmp(key, "autoresume")) fe.set.autoresume = v != 0;
        else if (!strcmp(key, "autofire")) fe.set.autofire = v != 0;
        else if (!strcmp(key, "slot")) fe.set.slot = clampi(v, 1, FE_SLOTS);
        else if (!strcmp(key, "inflives")) fe.set.inf_lives = v != 0;
        else if (!strcmp(key, "ffspeed")) fe.set.ff_speed = clampi(v, 2, 4);
        else if (!strcmp(key, "bfire") && v == 0) fe.set.map[FE_MAP_B] = FE_ACT_MENU; /* old file */
        else
            for (int k = 0; k < FE_MAP_COUNT; k++)
                if (!strcmp(key, map_key[k]))
                    fe.set.map[k] = clampi(v, 0, FE_ACT_COUNT - 1);
    }
    fclose(f);
}

/* write name.tmp, then rename: a crash never leaves half a file */
static FILE *open_tmp(const char *name, char *final, size_t n, char *tmp, size_t tn)
{
    path(final, n, name);
    snprintf(tmp, tn, "%s.tmp", final);
    return fopen(tmp, "wb");
}

static void settings_save(void)
{
    char p[600], tmp[620];
    FILE *f = open_tmp("settings.txt", p, sizeof p, tmp, sizeof tmp);
    if (!f)
        return;
    fprintf(f, "scale %d\nfilter %d\nborder %d\nsound %d\nvolume %d\nautoresume %d\n"
               "autofire %d\nslot %d\ninflives %d\nffspeed %d\n",
            fe.set.scale, fe.set.filter, fe.set.border, fe.set.sound, fe.set.volume,
            fe.set.autoresume, fe.set.autofire, fe.set.slot, fe.set.inf_lives, fe.set.ff_speed);
    for (int k = 0; k < FE_MAP_COUNT; k++)
        fprintf(f, "%s %d\n", map_key[k], fe.set.map[k]);
    if (fclose(f) == 0)
        rename(tmp, p);
    else
        remove(tmp);
}

static void best_load(void)
{
    char p[600];
    path(p, sizeof p, "hiscore.txt");
    FILE *f = fopen(p, "r");
    unsigned v = 0;
    if (f) {
        if (fscanf(f, "%u", &v) != 1 || v > 999999)
            v = 0;
        fclose(f);
    }
    fe.best = v;
}

static void best_save(void)
{
    char p[600], tmp[620];
    FILE *f = open_tmp("hiscore.txt", p, sizeof p, tmp, sizeof tmp);
    if (!f)
        return;
    fprintf(f, "%u\n", (unsigned)fe.best);
    if (fclose(f) == 0)
        rename(tmp, p);
    else
        remove(tmp);
}

static void slot_name(char *out, size_t n, int slot)
{
    snprintf(out, n, "state%d.bin", slot);
}

/* A state file: magic, size of the machine state, game version, state. The
 * size and version make sure a state from another build is never loaded. */
static int state_save(const char *name)
{
    if (!fe.game_ok)
        return -1;
    char p[600], tmp[620];
    FILE *f = open_tmp(name, p, sizeof p, tmp, sizeof tmp);
    if (!f)
        return -1;
    lj_state_save(fe.state_buf);
    char hdr[64] = {0};
    snprintf(hdr, sizeof hdr, "%s %zu %.40s", STATE_MAGIC, fe.state_size, lj_game_version());
    int ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr &&
             fwrite(fe.state_buf, 1, fe.state_size, f) == fe.state_size;
    if (fclose(f) != 0)
        ok = 0;
    if (!ok) {
        remove(tmp);
        return -1;
    }
    return rename(tmp, p);
}

static void hiscore_baseline(void)
{
    fe.hi_prev = lj_hiscore();
    fe.hi_pending = 0;
}

static int state_load(const char *name)
{
    if (!fe.game_ok)
        return -1;
    char p[600];
    path(p, sizeof p, name);
    FILE *f = fopen(p, "rb");
    if (!f)
        return -1;
    char hdr[64], want[64] = {0};
    snprintf(want, sizeof want, "%s %zu %.40s", STATE_MAGIC, fe.state_size, lj_game_version());
    int ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && memcmp(hdr, want, sizeof hdr) == 0;
    if (ok)
        ok = fread(fe.state_buf, 1, fe.state_size, f) == fe.state_size;
    fclose(f);
    if (!ok)
        return -1;
    if (lj_state_load(fe.state_buf, fe.state_size) != 0)
        return -1;
    /* input comes from the controller now, not from the saved state */
    lj_clear_keys();
    lj_set_joystick(1, 0);
    lj_set_joystick(2, 0);
    fe.key_p = fe.key_m = 0;
    /* a loaded high score is not a new record */
    hiscore_baseline();
    return 0;
}

static int slot_used(int slot)
{
    char name[32], p[600];
    slot_name(name, sizeof name, slot);
    path(p, sizeof p, name);
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* ---- high score -------------------------------------------------------- */

/* Called after every emulated frame. The game keeps its high score only
 * until power off; this keeps the best one in hiscore.txt.
 *  - Save: a new value must be the same in two frames in a row (the game
 *    copies it byte by byte; a frame can end in the middle of the copy)
 *    and must come from a game without infinite lives.
 *  - Restore: only while the score is 0. Then the game cannot be in its
 *    copy routine (it copies only when the score is higher). */
static void hiscore_sync(void)
{
    if (!fe.extras)
        return;
    uint32_t score = lj_score();
    uint32_t hi = lj_hiscore();
    if (score == 0) {
        fe.cheated = fe.set.inf_lives; /* a new game starts now */
        if (hi < fe.best || (fe.hi_force && hi != fe.best)) {
            lj_set_hiscore(fe.best);
            hi = fe.best;
        }
        fe.hi_force = 0;
    } else if (fe.set.inf_lives) {
        fe.cheated = 1;
    }
    if (hi != fe.hi_prev) {
        fe.hi_pending = !fe.cheated;
        fe.hi_prev = hi;
    } else if (fe.hi_pending) {
        fe.hi_pending = 0;
        if (hi > fe.best) {
            fe.best = hi;
            best_save();
        }
    }
}

/* ---- drawing ---------------------------------------------------------- */

static void toast(const char *msg)
{
    snprintf(fe.toast, sizeof fe.toast, "%s", msg);
    fe.toast_frames = 100;
}

static void draw_text(int x, int y, const char *s, uint32_t col)
{
    for (; *s; s++, x += 8) {
        unsigned c = (unsigned char)*s;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c < 0x20 || c > 0x7E)
            c = '?';
        const uint8_t *g = font8x8[c - 0x20];
        for (int r = 0; r < 8; r++) {
            int yy = y + r;
            if (yy < 0 || yy >= FE_H)
                continue;
            for (int b = 0; b < 8; b++) {
                int xx = x + b;
                if (xx < 0 || xx >= FE_W)
                    continue;
                if (g[r] & (1 << b))
                    fe.rgba[yy * FE_W + xx] = col;
            }
        }
    }
}

static void draw_text_c(int y, const char *s, uint32_t col)
{
    int w = (int)strlen(s) * 8;
    draw_text((FE_W - w) / 2, y, s, col);
}

static void dim_rect(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t c = fe.rgba[y * FE_W + x];
            /* keep alpha, quarter brightness */
            fe.rgba[y * FE_W + x] = (c & 0xFF000000u) | ((c >> 2) & 0x003F3F3Fu);
        }
}

static void compose_game(void)
{
    const uint8_t *fb = lj_framebuffer();
    for (int i = 0; i < FE_W * FE_H; i++)
        fe.rgba[i] = fe.pal[fb[i] & 15];
}

static const char *item_text(int id, char *buf, size_t n)
{
    static const char *scale[] = {"FIT", "INTEGER", "STRETCH"};
    static const char *filter[] = {"SHARP", "NEAREST", "SMOOTH", "SCANLINES"};
    static const char *border[] = {"FULL", "SMALL", "NONE"};
    if (id >= I_MAP0 && id <= I_MAP_LAST) {
        int k = id - I_MAP0;
        snprintf(buf, n, "%-7s %s", map_name[k], act_name[fe.set.map[k]]);
        return buf;
    }
    switch (id) {
    case I_RESUME: return "RESUME";
    case I_SAVE: snprintf(buf, n, "SAVE STATE: SLOT %d", fe.set.slot); return buf;
    case I_LOAD:
        snprintf(buf, n, "LOAD STATE: SLOT %d%s", fe.set.slot, slot_used(fe.set.slot) ? "" : " (EMPTY)");
        return buf;
    case I_RESET: return fe.confirm == I_RESET ? "RESET GAME? PRESS A AGAIN" : "RESET GAME";
    case I_GO_VIDEO: return "VIDEO >";
    case I_GO_SOUND: return "SOUND >";
    case I_GO_CONTROLS: return "CONTROLS >";
    case I_GO_EXTRAS: return "ENHANCEMENTS >";
    case I_QUIT: return "QUIT";
    case I_SCALE: snprintf(buf, n, "SCREEN: %s", scale[fe.set.scale]); return buf;
    case I_FILTER: snprintf(buf, n, "FILTER: %s", filter[fe.set.filter]); return buf;
    case I_BORDER: snprintf(buf, n, "BORDER: %s", border[fe.set.border]); return buf;
    case I_SOUND: snprintf(buf, n, "SOUND: %s", fe.set.sound ? "ON" : "OFF"); return buf;
    case I_VOLUME: snprintf(buf, n, "VOLUME: %d", fe.set.volume); return buf;
    case I_AUTOFIRE: snprintf(buf, n, "AUTO FIRE: %s", fe.set.autofire ? "ON" : "OFF"); return buf;
    case I_DEFAULTS: return "DEFAULT CONTROLS";
    case I_LIVES:
        if (!fe.extras)
            return "INFINITE LIVES: NOT AVAILABLE";
        snprintf(buf, n, "INFINITE LIVES: %s", fe.set.inf_lives ? "ON" : "OFF");
        return buf;
    case I_FFSPEED: snprintf(buf, n, "FAST FORWARD: %dX", fe.set.ff_speed); return buf;
    case I_AUTORESUME:
        snprintf(buf, n, "RESUME ON START: %s", fe.set.autoresume ? "ON" : "OFF");
        return buf;
    case I_HISCORE:
        if (!fe.extras)
            return "HIGH SCORE: NOT AVAILABLE";
        if (fe.confirm == I_HISCORE)
            return "CLEAR HIGH SCORE? PRESS A AGAIN";
        snprintf(buf, n, "HIGH SCORE: %06u (A: CLEAR)", (unsigned)fe.best);
        return buf;
    case I_BACK: return "BACK";
    default: return "";
    }
}

static int is_option(int id)
{
    return id == I_SAVE || id == I_LOAD || (id >= I_SCALE && id <= I_MAP_LAST) ||
           id == I_AUTOFIRE || id == I_LIVES || id == I_FFSPEED || id == I_AUTORESUME;
}

static void draw_menu(void)
{
    dim_rect(0, 0, FE_W, FE_H);
    const uint32_t white = 0xFFFFFFFFu, grey = 0xFFB2B2B2u, yellow = 0xFF71F1EDu, blue = 0xFFEB6D70u;
    draw_text_c(16, pages[fe.page].title, yellow);
    char buf[64];
    for (int i = 0; i < pages[fe.page].n; i++) {
        int y = MENU_Y0 + i * MENU_DY;
        const char *t = item_text(pages[fe.page].items[i], buf, sizeof buf);
        if (i == fe.sel) {
            draw_text(56, y, ">", white);
            draw_text(72, y, t, white);
        } else {
            draw_text(72, y, t, grey);
        }
    }
    int cur = pages[fe.page].items[fe.sel];
    draw_text_c(236, is_option(cur) ? "LEFT/RIGHT: CHANGE   B: BACK" : "A: SELECT   B: BACK", blue);
    if (fe.page == P_CONTROLS)
        draw_text_c(248, "BACK KEY ALWAYS OPENS THE MENU", blue);
}

static void draw_message_screen(void)
{
    for (int i = 0; i < FE_W * FE_H; i++)
        fe.rgba[i] = 0xFF000000u;
    const uint32_t white = 0xFFFFFFFFu, yellow = 0xFF71F1EDu;
    draw_text_c(90, "LAZY JONES", yellow);
    draw_text_c(120, "THIS APP WAS BUILT WITHOUT", white);
    draw_text_c(132, "THE GAME FILE.", white);
    draw_text_c(156, "SEE README.MD: PUT YOUR COPY", white);
    draw_text_c(168, "OF THE GAME INTO game/ AND BUILD.", white);
    draw_text_c(200, "PRESS BACK TO QUIT", yellow);
}

/* ---- menu actions ------------------------------------------------------- */

static int wrap(int v, int count) { return ((v % count) + count) % count; }

static void quick_save(void)
{
    char name[32], msg[48];
    slot_name(name, sizeof name, fe.set.slot);
    if (state_save(name) == 0)
        snprintf(msg, sizeof msg, "SAVED TO SLOT %d", fe.set.slot);
    else
        snprintf(msg, sizeof msg, "SAVE FAILED");
    toast(msg);
}

static void quick_load(void)
{
    char name[32], msg[48];
    slot_name(name, sizeof name, fe.set.slot);
    if (state_load(name) == 0)
        snprintf(msg, sizeof msg, "LOADED SLOT %d", fe.set.slot);
    else
        snprintf(msg, sizeof msg, "SLOT %d IS EMPTY", fe.set.slot);
    toast(msg);
}

static void menu_change(int id, int dir)
{
    if (id >= I_MAP0 && id <= I_MAP_LAST) {
        int k = id - I_MAP0;
        fe.set.map[k] = wrap(fe.set.map[k] + dir, FE_ACT_COUNT);
        settings_save();
        return;
    }
    switch (id) {
    case I_SAVE: case I_LOAD: fe.set.slot = 1 + wrap(fe.set.slot - 1 + dir, FE_SLOTS); break;
    case I_SCALE: fe.set.scale = wrap(fe.set.scale + dir, FE_SCALE_COUNT); break;
    case I_FILTER: fe.set.filter = wrap(fe.set.filter + dir, FE_FILTER_COUNT); break;
    case I_BORDER: fe.set.border = wrap(fe.set.border + dir, FE_BORDER_COUNT); break;
    case I_SOUND: fe.set.sound = !fe.set.sound; break;
    case I_VOLUME: fe.set.volume = clampi(fe.set.volume + dir, 1, 10); break;
    case I_AUTOFIRE: fe.set.autofire = !fe.set.autofire; break;
    case I_LIVES:
        if (!fe.extras)
            return;
        fe.set.inf_lives = !fe.set.inf_lives;
        break;
    case I_FFSPEED: fe.set.ff_speed = 2 + wrap(fe.set.ff_speed - 2 + dir, 3); break;
    case I_AUTORESUME: fe.set.autoresume = !fe.set.autoresume; break;
    default: return;
    }
    settings_save();
}

static void go_page(int page)
{
    fe.page = page;
    fe.sel = 0;
}

static void menu_back(void)
{
    fe.confirm = -1;
    if (fe.page == P_MAIN) {
        fe.menu_open = 0;
        return;
    }
    int go = pages[fe.page].go_item;
    fe.page = P_MAIN;
    fe.sel = 0;
    for (int i = 0; i < pages[P_MAIN].n; i++)
        if (pages[P_MAIN].items[i] == go)
            fe.sel = i;
}

static void menu_activate(int id)
{
    if (id != fe.confirm)
        fe.confirm = -1;
    switch (id) {
    case I_RESUME:
        fe.menu_open = 0;
        break;
    case I_SAVE:
        quick_save();
        fe.menu_open = 0;
        break;
    case I_LOAD:
        quick_load();
        fe.menu_open = 0;
        break;
    case I_RESET:
        if (fe.confirm != I_RESET) {
            fe.confirm = I_RESET;
            break;
        }
        fe.confirm = -1;
        if (fe.game_ok) {
            lj_game_init(fe.rate);
            fe.key_p = fe.key_m = 0;
            hiscore_baseline();
            toast("GAME RESET");
        }
        fe.menu_open = 0;
        break;
    case I_GO_VIDEO: go_page(P_VIDEO); break;
    case I_GO_SOUND: go_page(P_SOUND); break;
    case I_GO_CONTROLS: go_page(P_CONTROLS); break;
    case I_GO_EXTRAS: go_page(P_EXTRAS); break;
    case I_QUIT:
        if (fe.game_ok)
            state_save("autosave.bin");
        fe.quit = 1;
        break;
    case I_DEFAULTS:
        controls_defaults();
        settings_save();
        break;
    case I_HISCORE:
        if (!fe.extras)
            break;
        if (fe.confirm != I_HISCORE) {
            fe.confirm = I_HISCORE;
            break;
        }
        fe.confirm = -1;
        fe.best = 0;
        fe.hi_force = 1; /* the game's copy is cleared when the score is 0 */
        best_save();
        break;
    case I_BACK:
        menu_back();
        break;
    default:
        menu_change(id, 1);
        break;
    }
}

int fe_menu_tap(int x, int y)
{
    (void)x;
    if (!fe.menu_open)
        return 0;
    int i = (y - MENU_Y0 + 2) / MENU_DY;
    if (y < MENU_Y0 - 2 || i < 0 || i >= pages[fe.page].n)
        return 0;
    if (fe.sel != i)
        fe.confirm = -1;
    fe.sel = i;
    menu_activate(pages[fe.page].items[i]);
    return 1;
}

static void open_menu(void)
{
    fe.menu_open = 1;
    fe.page = P_MAIN;
    fe.sel = 0;
    fe.confirm = -1;
}

/* ---- public ------------------------------------------------------------ */

int fe_init(const char *data_dir, int sample_rate)
{
    memset(&fe, 0, sizeof fe);
    fe.confirm = -1;
    snprintf(fe.dir, sizeof fe.dir, "%s", data_dir ? data_dir : ".");
    settings_defaults();
    settings_load();
    best_load();
    for (int i = 0; i < 16; i++) {
        uint32_t c = lj_palette[i];
        fe.pal[i] = 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
    }
    fe.rate = sample_rate > 0 ? sample_rate : 48000;
    fe.game_ok = lj_game_init(fe.rate) == 0;
    if (fe.game_ok) {
        fe.state_size = lj_state_size();
        fe.state_buf = malloc(fe.state_size);
        if (!fe.state_buf)
            fe.game_ok = 0;
    }
    fe.extras = fe.game_ok && lj_extras_available();
    hiscore_baseline();
    if (fe.game_ok && fe.set.autoresume) {
        if (state_load("autosave.bin") == 0)
            toast("WELCOME BACK");
    }
    if (fe.game_ok)
        compose_game();
    else
        draw_message_screen();
    return fe.game_ok ? 0 : -1;
}

void fe_set_sample_rate(double rate) { lj_set_sample_rate(rate); }

void fe_set_output_rate(int rate)
{
    fe.rate = rate;
    lj_set_sample_rate(rate);
}

void fe_set_input(const fe_input_t *in) { fe.in = *in; }

void fe_touch_menu(void)
{
    if (fe.game_ok && !fe.menu_open)
        open_menu();
}

static uint8_t joystick_dirs(const fe_input_t *in)
{
    uint8_t j = 0;
    const float dz = 0.4f;
    if ((in->held & FE_BTN_UP) || in->ay < -dz) j |= LJ_JOY_UP;
    if ((in->held & FE_BTN_DOWN) || in->ay > dz) j |= LJ_JOY_DOWN;
    if ((in->held & FE_BTN_LEFT) || in->ax < -dz) j |= LJ_JOY_LEFT;
    if ((in->held & FE_BTN_RIGHT) || in->ax > dz) j |= LJ_JOY_RIGHT;
    /* opposite directions cannot both be closed on a real joystick */
    if ((j & LJ_JOY_UP) && (j & LJ_JOY_DOWN)) j &= (uint8_t)~(LJ_JOY_UP | LJ_JOY_DOWN);
    if ((j & LJ_JOY_LEFT) && (j & LJ_JOY_RIGHT)) j &= (uint8_t)~(LJ_JOY_LEFT | LJ_JOY_RIGHT);
    return j;
}

#define ACT(a) (1u << (a))

/* menu input: fixed buttons, whatever the mapping is */
static void menu_input(uint32_t held, uint32_t pressed)
{
    uint32_t back = FE_BTN_B | FE_BTN_SELECT | FE_BTN_MENU;
    for (int k = 0; k < FE_MAP_COUNT; k++)
        if (fe.set.map[k] == FE_ACT_MENU)
            back |= map_btn[k];
    uint32_t activate = (FE_BTN_A | FE_BTN_X | FE_BTN_Y | FE_BTN_START) & ~back;

    /* auto repeat for the directions */
    uint32_t dirs = held & (FE_BTN_UP | FE_BTN_DOWN | FE_BTN_LEFT | FE_BTN_RIGHT);
    if (dirs && dirs == fe.repeat_btn) {
        if (++fe.repeat_count > 18 && (fe.repeat_count & 3) == 0)
            pressed |= dirs;
    } else {
        fe.repeat_btn = dirs;
        fe.repeat_count = 0;
    }
    float ay = fe.in.ay;
    if (ay < -0.6f || ay > 0.6f) {
        if (!fe.stick_latch)
            pressed |= ay < 0 ? FE_BTN_UP : FE_BTN_DOWN;
        fe.stick_latch = 1;
    } else if (ay > -0.3f && ay < 0.3f) {
        fe.stick_latch = 0;
    }
    int n = pages[fe.page].n;
    if (pressed & FE_BTN_UP) {
        fe.sel = wrap(fe.sel - 1, n);
        fe.confirm = -1;
    }
    if (pressed & FE_BTN_DOWN) {
        fe.sel = wrap(fe.sel + 1, n);
        fe.confirm = -1;
    }
    int id = pages[fe.page].items[fe.sel];
    if (pressed & FE_BTN_LEFT)
        menu_change(id, -1);
    if (pressed & FE_BTN_RIGHT)
        menu_change(id, 1);
    if (pressed & activate)
        menu_activate(id);
    else if (pressed & back)
        menu_back();
}

static void run_game_frame(int keep_audio)
{
    if (fe.extras)
        lj_cheat_infinite_lives(fe.set.inf_lives);
    lj_run_frame();
    hiscore_sync();
    if (!keep_audio) {
        int16_t drop[1024];
        while (lj_audio_read(drop, 1024) > 0) {
        }
    }
}

int fe_frame(void)
{
    uint32_t held = fe.in.held;
    uint32_t pressed = held & ~fe.prev_held;
    fe.prev_held = held;

    if (!fe.game_ok) {
        if (pressed & (FE_BTN_MENU | FE_BTN_SELECT | FE_BTN_B))
            fe.quit = 1;
        draw_message_screen();
        return 0;
    }

    /* actions of the mapped buttons */
    uint32_t act_held = 0, act_pressed = 0;
    for (int k = 0; k < FE_MAP_COUNT; k++) {
        if (held & map_btn[k])
            act_held |= ACT(fe.set.map[k]);
        if (pressed & map_btn[k])
            act_pressed |= ACT(fe.set.map[k]);
    }
    if (held & FE_BTN_TOUCH_FIRE)
        act_held |= ACT(FE_ACT_FIRE);

    if (!fe.menu_open && ((pressed & FE_BTN_MENU) || (act_pressed & ACT(FE_ACT_MENU)))) {
        open_menu();
        pressed = 0;
    }

    if (fe.menu_open) {
        lj_set_joystick(2, 0);
        lj_clear_keys();
        fe.key_p = fe.key_m = 0; /* sent again after the menu if still held */
        fe.fire_count = 0;
        fe.fast = 0;
        menu_input(held, pressed);
        compose_game();
        if (fe.menu_open)
            draw_menu();
        if (fe.toast_frames > 0 && !fe.menu_open) {
            fe.toast_frames--;
            draw_text_c(FE_H - 20, fe.toast, 0xFFFFFFFFu);
        }
        return 0;
    }

    if (act_pressed & ACT(FE_ACT_SAVE))
        quick_save();
    if (act_pressed & ACT(FE_ACT_LOAD))
        quick_load();

    /* joystick, with auto fire: 3 frames pressed, 3 released (8 per second) */
    uint8_t joy = joystick_dirs(&fe.in);
    if (act_held & ACT(FE_ACT_FIRE)) {
        if (!fe.set.autofire || fe.fire_count % 6 < 3)
            joy |= LJ_JOY_FIRE;
        fe.fire_count++;
    } else {
        fe.fire_count = 0;
    }
    lj_set_joystick(2, joy);
    int p = (act_held & ACT(FE_ACT_PAUSE)) != 0;
    int m = (act_held & ACT(FE_ACT_MUSIC)) != 0;
    if (p != fe.key_p)
        lj_set_key(4 + 1, 1, p);       /* P: column 5, row 1 */
    if (m != fe.key_m)
        lj_set_key(4, 4, m);           /* M: column 4, row 4 */
    fe.key_p = p;
    fe.key_m = m;

    /* fast forward: the extra frames are not heard */
    fe.fast = (act_held & ACT(FE_ACT_FAST)) != 0;
    int n = fe.fast ? fe.set.ff_speed : 1;
    for (int i = 0; i < n; i++)
        run_game_frame(i == n - 1);

    compose_game();
    if (fe.fast) {
        char buf[16];
        snprintf(buf, sizeof buf, ">> %dX", fe.set.ff_speed);
        draw_text(8, 8, buf, 0xFFFFFFFFu);
    }
    if (fe.toast_frames > 0) {
        fe.toast_frames--;
        draw_text_c(FE_H - 20, fe.toast, 0xFFFFFFFFu);
    }
    return 1;
}

const uint32_t *fe_rgba(void) { return fe.rgba; }

void fe_view(int *x, int *y, int *w, int *h)
{
    switch (fe.set.border) {
    case FE_BORDER_NONE:
        *x = 32; *y = 35; *w = 320; *h = 200;
        break;
    case FE_BORDER_SMALL:
        *x = 16; *y = 19; *w = 352; *h = 232;
        break;
    default:
        *x = 0; *y = 0; *w = FE_W; *h = FE_H;
        break;
    }
}

const fe_settings_t *fe_get_settings(void) { return &fe.set; }

size_t fe_audio_read(int16_t *out, size_t max)
{
    size_t n = lj_audio_read(out, max);
    int vol = fe.set.sound && !fe.menu_open ? fe.set.volume : 0;
    for (size_t i = 0; i < n; i++)
        out[i] = (int16_t)((int32_t)out[i] * vol / 10);
    return n;
}

int fe_audio_wanted(void) { return fe.game_ok && fe.set.sound; }

void fe_pause(void)
{
    settings_save();
    if (fe.game_ok)
        state_save("autosave.bin");
}

int fe_quit_requested(void) { return fe.quit; }

int fe_menu_open(void) { return fe.menu_open; }

uint32_t fe_best_score(void) { return fe.best; }
