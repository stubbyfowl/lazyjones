/*
 * fe.c - portable front end (see fe.h).
 *
 * Controls (defaults):
 *   D-pad / left stick ... joystick (port 2)
 *   A, X, Y .............. fire  (B too, unless set to open the menu)
 *   START ................ the game's pause key (P)
 *   R1 ................... the game's music key (M)
 *   SELECT, Back, Menu ... this menu
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fe.h"
#include "../runtime/lj.h"
#include "font8x8.h"

#define STATE_MAGIC "LJST0001"
#define MENU_ROWS 12

enum {
    M_RESUME, M_SAVE, M_LOAD, M_RESET, M_SCALE, M_FILTER, M_BORDER,
    M_SOUND, M_VOLUME, M_BBUTTON, M_AUTORESUME, M_QUIT, M_COUNT
};

static struct {
    char dir[512];
    int game_ok;
    fe_settings_t set;
    fe_input_t in;
    uint32_t prev_held;
    int menu_open;
    int menu_sel;
    int confirm_reset;
    int quit;
    int repeat_btn, repeat_count;
    char toast[48];
    int toast_frames;
    int key_p, key_m;          /* game keys held this frame */
    uint32_t rgba[FE_W * FE_H];
    uint32_t pal[16];
    uint8_t *state_buf;
    size_t state_size;
    int rate;               /* output sample rate */
} fe;

/* ---- files ----------------------------------------------------------- */

static void path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", fe.dir, name);
}

static void settings_defaults(void)
{
    fe.set.scale = FE_SCALE_FIT;
    fe.set.filter = FE_FILTER_SHARP;
    fe.set.border = FE_BORDER_FULL;
    fe.set.sound = 1;
    fe.set.volume = 8;
    fe.set.autoresume = 1;
    fe.set.bfire = 1;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

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
        else if (!strcmp(key, "bfire")) fe.set.bfire = v != 0;
    }
    fclose(f);
}

static void settings_save(void)
{
    char p[600], tmp[620];
    path(p, sizeof p, "settings.txt");
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f, "scale %d\nfilter %d\nborder %d\nsound %d\nvolume %d\nautoresume %d\nbfire %d\n",
            fe.set.scale, fe.set.filter, fe.set.border, fe.set.sound, fe.set.volume,
            fe.set.autoresume, fe.set.bfire);
    if (fclose(f) == 0)
        rename(tmp, p);
}

/* A state file: magic, size of the machine state, game hash, state. The
 * hash and size make sure a state from another build is never loaded. */
static int state_save(const char *name)
{
    if (!fe.game_ok)
        return -1;
    char p[600], tmp[620];
    path(p, sizeof p, name);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
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
    return 0;
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

static const char *opt_name(int item, char *buf, size_t n)
{
    static const char *scale[] = {"FIT", "INTEGER", "STRETCH"};
    static const char *filter[] = {"SHARP", "NEAREST", "SMOOTH"};
    static const char *border[] = {"FULL", "SMALL", "NONE"};
    switch (item) {
    case M_RESUME: return "RESUME";
    case M_SAVE: return "SAVE STATE";
    case M_LOAD: return "LOAD STATE";
    case M_RESET: return fe.confirm_reset ? "RESET GAME? PRESS A AGAIN" : "RESET GAME";
    case M_SCALE: snprintf(buf, n, "SCREEN: %s", scale[fe.set.scale]); return buf;
    case M_FILTER: snprintf(buf, n, "FILTER: %s", filter[fe.set.filter]); return buf;
    case M_BORDER: snprintf(buf, n, "BORDER: %s", border[fe.set.border]); return buf;
    case M_SOUND: snprintf(buf, n, "SOUND: %s", fe.set.sound ? "ON" : "OFF"); return buf;
    case M_VOLUME: snprintf(buf, n, "VOLUME: %d", fe.set.volume); return buf;
    case M_BBUTTON: snprintf(buf, n, "B BUTTON: %s", fe.set.bfire ? "FIRE" : "MENU"); return buf;
    case M_AUTORESUME: snprintf(buf, n, "RESUME ON START: %s", fe.set.autoresume ? "ON" : "OFF"); return buf;
    case M_QUIT: return "QUIT";
    default: return "";
    }
}

#define MENU_Y0 44
#define MENU_DY 12

static void draw_menu(void)
{
    dim_rect(0, 0, FE_W, FE_H);
    const uint32_t white = 0xFFFFFFFFu, grey = 0xFFB2B2B2u, yellow = 0xFF71F1EDu, blue = 0xFFEB6D70u;
    draw_text_c(16, "LAZY JONES", yellow);
    char buf[64];
    for (int i = 0; i < M_COUNT; i++) {
        int y = MENU_Y0 + i * MENU_DY;
        const char *t = opt_name(i, buf, sizeof buf);
        if (i == fe.menu_sel) {
            draw_text(56, y, ">", white);
            draw_text(72, y, t, white);
        } else {
            draw_text(72, y, t, grey);
        }
    }
    draw_text_c(MENU_Y0 + M_COUNT * MENU_DY + 8, "START=PAUSE  R1=MUSIC ON/OFF", blue);
    draw_text_c(MENU_Y0 + M_COUNT * MENU_DY + 20, "SELECT / BACK = THIS MENU", blue);
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

static void menu_change(int item, int dir)
{
    switch (item) {
    case M_SCALE: fe.set.scale = (fe.set.scale + FE_SCALE_COUNT + dir) % FE_SCALE_COUNT; break;
    case M_FILTER: fe.set.filter = (fe.set.filter + FE_FILTER_COUNT + dir) % FE_FILTER_COUNT; break;
    case M_BORDER: fe.set.border = (fe.set.border + FE_BORDER_COUNT + dir) % FE_BORDER_COUNT; break;
    case M_SOUND: fe.set.sound = !fe.set.sound; break;
    case M_VOLUME: fe.set.volume = clampi(fe.set.volume + (dir ? dir : 1), 1, 10); break;
    case M_BBUTTON: fe.set.bfire = !fe.set.bfire; break;
    case M_AUTORESUME: fe.set.autoresume = !fe.set.autoresume; break;
    default: break;
    }
    settings_save();
}

static void menu_activate(int item)
{
    if (item != M_RESET)
        fe.confirm_reset = 0;
    switch (item) {
    case M_RESUME:
        fe.menu_open = 0;
        break;
    case M_SAVE:
        toast(state_save("state1.bin") == 0 ? "STATE SAVED" : "SAVE FAILED");
        fe.menu_open = 0;
        break;
    case M_LOAD:
        toast(state_load("state1.bin") == 0 ? "STATE LOADED" : "NO SAVED STATE");
        fe.menu_open = 0;
        break;
    case M_RESET:
        if (!fe.confirm_reset) {
            fe.confirm_reset = 1;
            break;
        }
        fe.confirm_reset = 0;
        if (fe.game_ok) {
            lj_game_init(fe.rate);
            fe.key_p = fe.key_m = 0;
            toast("GAME RESET");
        }
        fe.menu_open = 0;
        break;
    case M_QUIT:
        if (fe.game_ok)
            state_save("autosave.bin");
        fe.quit = 1;
        break;
    default:
        menu_change(item, 1);
        break;
    }
}

int fe_menu_tap(int x, int y)
{
    (void)x;
    if (!fe.menu_open)
        return 0;
    int i = (y - MENU_Y0 + 2) / MENU_DY;
    if (y < MENU_Y0 - 2 || i < 0 || i >= M_COUNT)
        return 0;
    if (fe.menu_sel != i && i != M_RESET)
        fe.confirm_reset = 0;
    fe.menu_sel = i;
    menu_activate(i);
    return 1;
}

/* ---- public ------------------------------------------------------------ */

int fe_init(const char *data_dir, int sample_rate)
{
    memset(&fe, 0, sizeof fe);
    snprintf(fe.dir, sizeof fe.dir, "%s", data_dir ? data_dir : ".");
    settings_defaults();
    settings_load();
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
    if (fe.game_ok && !fe.menu_open) {
        fe.menu_open = 1;
        fe.menu_sel = 0;
        fe.confirm_reset = 0;
    }
}

static uint8_t joystick_bits(const fe_input_t *in, int bfire)
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
    uint32_t fire = FE_BTN_FIRE | FE_BTN_FIRE3 | (bfire ? FE_BTN_FIRE2 : 0);
    if (in->held & fire) j |= LJ_JOY_FIRE;
    return j;
}

int fe_frame(void)
{
    uint32_t held = fe.in.held;
    uint32_t pressed = held & ~fe.prev_held;
    fe.prev_held = held;

    if (!fe.game_ok) {
        if (pressed & (FE_BTN_MENU | FE_BTN_SELECT | FE_BTN_FIRE2))
            fe.quit = 1;
        draw_message_screen();
        return 0;
    }

    uint32_t menu_btn = FE_BTN_MENU | FE_BTN_SELECT | (fe.set.bfire ? 0 : FE_BTN_FIRE2);
    if (!fe.menu_open && (pressed & menu_btn)) {
        fe.menu_open = 1;
        fe.menu_sel = 0;
        fe.confirm_reset = 0;
        pressed = 0;
    }

    if (fe.menu_open) {
        lj_set_joystick(2, 0);
        lj_clear_keys();
        /* auto repeat for up/down/left/right */
        uint32_t dirs = held & (FE_BTN_UP | FE_BTN_DOWN | FE_BTN_LEFT | FE_BTN_RIGHT);
        if (dirs && dirs == (uint32_t)fe.repeat_btn) {
            if (++fe.repeat_count > 18 && (fe.repeat_count & 3) == 0)
                pressed |= dirs;
        } else {
            fe.repeat_btn = (int)dirs;
            fe.repeat_count = 0;
        }
        float ay = fe.in.ay;
        static int stick_latch;
        if (ay < -0.6f || ay > 0.6f) {
            if (!stick_latch)
                pressed |= ay < 0 ? FE_BTN_UP : FE_BTN_DOWN;
            stick_latch = 1;
        } else if (ay > -0.3f && ay < 0.3f) {
            stick_latch = 0;
        }
        if (pressed & FE_BTN_UP) {
            fe.menu_sel = (fe.menu_sel + M_COUNT - 1) % M_COUNT;
            fe.confirm_reset = 0;
        }
        if (pressed & FE_BTN_DOWN) {
            fe.menu_sel = (fe.menu_sel + 1) % M_COUNT;
            fe.confirm_reset = 0;
        }
        if (pressed & FE_BTN_LEFT)
            menu_change(fe.menu_sel, -1);
        if (pressed & FE_BTN_RIGHT)
            menu_change(fe.menu_sel, 1);
        if (pressed & (FE_BTN_FIRE | FE_BTN_FIRE3 | FE_BTN_START))
            menu_activate(fe.menu_sel);
        else if (pressed & (FE_BTN_MENU | FE_BTN_SELECT | FE_BTN_FIRE2))
            fe.menu_open = 0;
        compose_game();
        if (fe.menu_open)
            draw_menu();
        if (fe.toast_frames > 0 && !fe.menu_open) {
            fe.toast_frames--;
            draw_text_c(FE_H - 20, fe.toast, 0xFFFFFFFFu);
        }
        return 0;
    }

    /* game input */
    lj_set_joystick(2, joystick_bits(&fe.in, fe.set.bfire));
    int p = (held & FE_BTN_START) != 0;
    int m = (held & FE_BTN_R) != 0;
    if (p != fe.key_p)
        lj_set_key(4 + 1, 1, p);       /* P: column 5, row 1 */
    if (m != fe.key_m)
        lj_set_key(4, 4, m);           /* M: column 4, row 4 */
    fe.key_p = p;
    fe.key_m = m;

    lj_run_frame();
    compose_game();
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
