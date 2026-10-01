/*
 * fe_test.c - tests of the front end (frontend/fe.c) on the host: menu
 * pages, save states and slots, settings, button mapping, auto fire, fast
 * forward, infinite lives, the saved high score, autosave, touch, volume.
 *
 * Build and run with "make -C host fetest" (needs the recompiled game in
 * build/gen) or "make -C host fetest_nogame" (no game file needed).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "c64.h"
#include "fe.h"
#include "lj.h"

uint32_t lj_debug_hash(void);

static int fails, checks;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { \
        fails++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static char dir[256];
static fe_input_t in;
static size_t audio_samples;
static long audio_abs_sum;

/* one frame with the buttons in 'held', then audio drained */
static int frame(uint32_t held)
{
    in.held = held;
    fe_set_input(&in);
    int r = fe_frame();
    int16_t buf[4096];
    size_t n;
    while ((n = fe_audio_read(buf, 4096)) > 0) {
        audio_samples += n;
        for (size_t i = 0; i < n; i++)
            audio_abs_sum += buf[i] < 0 ? -buf[i] : buf[i];
    }
    return r;
}

/* press and release a button (two frames) */
static void press(uint32_t b)
{
    frame(b);
    frame(0);
}

static int count_color(uint32_t c, int x0, int y0, int x1, int y1)
{
    const uint32_t *px = fe_rgba();
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            n += px[y * FE_W + x] == c;
    return n;
}

#define WHITE 0xFFFFFFFFu

#ifndef LJ_NOGAME
static void frames(int n, uint32_t held)
{
    for (int i = 0; i < n; i++)
        frame(held);
}

static int file_exists(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    struct stat st;
    return stat(p, &st) == 0;
}

static void remove_file(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    remove(p);
}

static void write_file(const char *name, const char *text)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static long read_number(const char *name, const char *key)
{
    char p[512], k[32];
    long v, found = -1;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "r");
    if (!f)
        return -1;
    if (!key) {
        if (fscanf(f, "%ld", &v) == 1)
            found = v;
    } else {
        while (fscanf(f, "%31s %ld", k, &v) == 2)
            if (!strcmp(k, key))
                found = v;
    }
    fclose(f);
    return found;
}

/* menu rows (as in fe.c) */
enum { M_RESUME, M_SAVE, M_LOAD, M_RESET, M_VIDEO, M_SOUND, M_CONTROLS, M_EXTRAS, M_QUIT };
enum { V_SCALE, V_FILTER, V_BORDER, V_BACK };
enum { S_SOUND, S_VOLUME, S_BACK };
enum { C_MAP0, C_AUTOFIRE = 10, C_DEFAULTS, C_BACK };
enum { E_LIVES, E_FFSPEED, E_AUTORESUME, E_HISCORE, E_BACK };

static void menu_close(void)
{
    for (int i = 0; i < 4 && fe_menu_open(); i++)
        press(FE_BTN_B); /* B: back to the main page, then close */
}

/* open the menu on 'page' (-1: main page) and move to 'row' */
static void menu_goto(int page, int row)
{
    menu_close();
    press(FE_BTN_SELECT);
    if (page >= 0) {
        for (int i = 0; i < page; i++)
            press(FE_BTN_DOWN);
        press(FE_BTN_A);
    }
    for (int i = 0; i < row; i++)
        press(FE_BTN_DOWN);
}

/* from power on: title, controls, lives question, game (5 lives) */
static void start_game(void)
{
    frames(400, 0);
    frames(10, FE_BTN_A);
    frames(190, 0);
    frames(10, FE_BTN_A);
    frames(190, 0);
    frames(10, FE_BTN_A);
    frames(100, 0);
}

static void fresh_start(void)
{
    remove_file("autosave.bin");
    CHECK(fe_init(dir, 48000) == 0, "fe_init");
}

static void set_score(unsigned v)
{
    for (int i = 0; i < 3; i++) {
        unsigned two = v % 100;
        lj_poke((uint16_t)(0x0360 + i), (uint8_t)(((two / 10) << 4) | (two % 10)));
        v /= 100;
    }
}

/* the six digits of HI on the screen, as text */
static void hi_on_screen(char out[7])
{
    for (int i = 0; i < 6; i++)
        out[i] = (char)lj_peek((uint16_t)(0x07E0 + i));
    out[6] = 0;
}

static void test_basics(void)
{
    remove_file("settings.txt");
    remove_file("hiscore.txt");
    for (int s = 1; s <= 4; s++) {
        char n[32];
        snprintf(n, sizeof n, "state%d.bin", s);
        remove_file(n);
    }
    fresh_start();
    const fe_settings_t *s = fe_get_settings();
    CHECK(s->scale == FE_SCALE_FIT && s->filter == FE_FILTER_SHARP && s->border == FE_BORDER_FULL &&
          s->sound == 1 && s->volume == 8 && s->autoresume == 1, "default settings");
    CHECK(s->map[FE_MAP_A] == FE_ACT_FIRE && s->map[FE_MAP_B] == FE_ACT_FIRE &&
          s->map[FE_MAP_L1] == FE_ACT_FAST && s->map[FE_MAP_R1] == FE_ACT_MUSIC &&
          s->map[FE_MAP_START] == FE_ACT_PAUSE && s->map[FE_MAP_SELECT] == FE_ACT_MENU &&
          s->autofire == 0 && s->slot == 1 && s->inf_lives == 0 && s->ff_speed == 3,
          "default controls and enhancements");
    CHECK(lj_extras_available(), "game hooks available for this game");
    CHECK(!fe_menu_open(), "menu closed at start");

    /* frames and sound: 50.1245 frames and 48000 samples per second */
    audio_samples = 0;
    audio_abs_sum = 0;
    unsigned f0 = lj_frame_count();
    frames(500, 0);
    CHECK(lj_frame_count() - f0 == 500, "500 game frames ran (%u)", lj_frame_count() - f0);
    double want = 500 * 48000.0 / LJ_FRAME_HZ;
    CHECK(audio_samples > want - 2000 && audio_samples < want + 2000,
          "sound samples %zu, want about %.0f", audio_samples, want);
    CHECK(audio_abs_sum > 0, "title music is not silent");

    /* joystick (port 2, active high bits) */
    frame(FE_BTN_LEFT | FE_BTN_A);
    CHECK(C.joy2 == (LJ_JOY_LEFT | LJ_JOY_FIRE), "left+A -> %02x", C.joy2);
    frame(FE_BTN_LEFT | FE_BTN_RIGHT | FE_BTN_UP);
    CHECK(C.joy2 == LJ_JOY_UP, "opposite directions cancel -> %02x", C.joy2);
    in.ax = 0.3f; in.ay = -0.9f;
    frame(0);
    CHECK(C.joy2 == LJ_JOY_UP, "stick: dead zone x, up y -> %02x", C.joy2);
    in.ax = 0.0f; in.ay = 0.0f;
    frame(FE_BTN_B);
    CHECK(C.joy2 == LJ_JOY_FIRE, "B is fire -> %02x", C.joy2);
    frame(FE_BTN_X);
    CHECK(C.joy2 == LJ_JOY_FIRE, "X is fire -> %02x", C.joy2);
    frame(FE_BTN_Y);
    CHECK(C.joy2 == LJ_JOY_FIRE, "Y is fire -> %02x", C.joy2);
    frame(FE_BTN_TOUCH_FIRE);
    CHECK(C.joy2 == LJ_JOY_FIRE, "touch fire -> %02x", C.joy2);
    frame(0);
    CHECK(C.joy2 == 0, "released -> %02x", C.joy2);

    /* START holds the P key (column 5, row 1), R1/R2 the M key (column 4, row 4) */
    frame(FE_BTN_START);
    CHECK(C.kb_matrix[5] & (1 << 1), "START presses P");
    frame(FE_BTN_R1);
    CHECK(!(C.kb_matrix[5] & (1 << 1)) && (C.kb_matrix[4] & (1 << 4)), "R1 releases P, presses M");
    frame(FE_BTN_R2);
    CHECK(C.kb_matrix[4] & (1 << 4), "R2 presses M");
    frame(0);
    CHECK(C.kb_matrix[4] == 0 && C.kb_matrix[5] == 0, "keys released");

    /* menu: SELECT or the Back key; game stops; no sound; no joystick */
    press(FE_BTN_SELECT);
    CHECK(fe_menu_open(), "SELECT opens the menu");
    f0 = lj_frame_count();
    audio_samples = 0;
    frames(20, 0);
    CHECK(lj_frame_count() == f0, "game paused in the menu");
    CHECK(audio_samples == 0, "no sound in the menu");
    CHECK(count_color(WHITE, 0, 0, FE_W, FE_H) > 50, "menu text drawn");
    frame(FE_BTN_A | FE_BTN_LEFT);
    CHECK(C.joy2 == 0, "no joystick input reaches the game in the menu");
    frame(0);
    menu_close();
    CHECK(!fe_menu_open(), "B closes the menu");
    press(FE_BTN_MENU);
    CHECK(fe_menu_open(), "Back key opens the menu");
    press(FE_BTN_MENU);
    CHECK(!fe_menu_open(), "Back key closes it");

    /* START held while the menu opens and closes: P is pressed again */
    frame(FE_BTN_START);
    frame(FE_BTN_START | FE_BTN_SELECT);
    CHECK(fe_menu_open(), "menu opened with START held");
    CHECK(!(C.kb_matrix[5] & (1 << 1)), "P released in the menu");
    frame(FE_BTN_START | FE_BTN_B);
    CHECK(!fe_menu_open(), "menu closed with START held");
    frame(FE_BTN_START);
    CHECK(C.kb_matrix[5] & (1 << 1), "P pressed again after the menu");
    frame(0);
    frame(0);
}

static void test_states(void)
{
    const fe_settings_t *s = fe_get_settings();
    /* save to slot 1 */
    menu_goto(-1, M_SAVE);
    press(FE_BTN_A);
    CHECK(!fe_menu_open(), "menu closes after save");
    CHECK(file_exists("state1.bin"), "state1.bin written");
    uint32_t h_saved = lj_debug_hash();
    unsigned fc_saved = lj_frame_count();
    frames(300, FE_BTN_RIGHT);
    CHECK(lj_debug_hash() != h_saved, "state changed");
    menu_goto(-1, M_LOAD);
    press(FE_BTN_A);
    CHECK(lj_frame_count() == fc_saved, "frame counter restored");
    CHECK(lj_debug_hash() == h_saved, "machine state restored");

    /* slot 3: RIGHT twice on the SAVE line */
    menu_goto(-1, M_SAVE);
    press(FE_BTN_RIGHT);
    press(FE_BTN_RIGHT);
    CHECK(s->slot == 3, "slot 3 (%d)", s->slot);
    CHECK(read_number("settings.txt", "slot") == 3, "slot saved in settings");
    press(FE_BTN_A);
    CHECK(file_exists("state3.bin"), "state3.bin written");
    uint32_t h3 = lj_debug_hash();
    unsigned fc3 = lj_frame_count();
    frames(100, FE_BTN_LEFT);
    menu_goto(-1, M_LOAD);
    press(FE_BTN_A);
    CHECK(lj_frame_count() == fc3 && lj_debug_hash() == h3, "slot 3 loaded");
    /* slot 4 is empty: nothing happens */
    menu_goto(-1, M_LOAD);
    press(FE_BTN_RIGHT);
    CHECK(s->slot == 4, "slot 4");
    unsigned fc = lj_frame_count();
    press(FE_BTN_A);
    CHECK(lj_frame_count() == fc + 1, "empty slot not loaded");
    /* back to slot 1 (wraps: 4 -> 1) */
    menu_goto(-1, M_SAVE);
    press(FE_BTN_RIGHT);
    CHECK(s->slot == 1, "slot wraps to 1 (%d)", s->slot);
    menu_close();

    /* a state from a different build is refused */
    {
        char p[512];
        snprintf(p, sizeof p, "%s/state1.bin", dir);
        FILE *f = fopen(p, "r+b");
        CHECK(f != NULL, "open state1.bin");
        if (f) {
            fseek(f, 9, SEEK_SET);
            fputc('9', f); /* change the size field */
            fclose(f);
        }
        frames(10, 0);
        fc = lj_frame_count();
        menu_goto(-1, M_LOAD);
        press(FE_BTN_A); /* the release frame runs one game frame */
        CHECK(lj_frame_count() == fc + 1, "bad state file not loaded");
    }

    /* reset needs two presses; moving away cancels */
    frames(50, 0);
    menu_goto(-1, M_RESET);
    press(FE_BTN_A);
    CHECK(fe_menu_open(), "first press only asks");
    fc = lj_frame_count();
    press(FE_BTN_A);
    CHECK(!fe_menu_open(), "second press resets");
    CHECK(lj_frame_count() < fc, "frame counter restarted");
    menu_goto(-1, M_RESET);
    press(FE_BTN_A);
    press(FE_BTN_DOWN);
    press(FE_BTN_UP);
    press(FE_BTN_A);
    CHECK(fe_menu_open(), "moving away cancels the reset question");
    menu_close();
}

static void test_settings_pages(void)
{
    const fe_settings_t *s = fe_get_settings();
    int x, y, w, h;
    /* video */
    menu_goto(M_VIDEO, V_BORDER);
    press(FE_BTN_RIGHT);
    fe_view(&x, &y, &w, &h);
    CHECK(s->border == FE_BORDER_SMALL && x == 16 && y == 19 && w == 352 && h == 232, "small border");
    press(FE_BTN_RIGHT);
    fe_view(&x, &y, &w, &h);
    CHECK(s->border == FE_BORDER_NONE && x == 32 && y == 35 && w == 320 && h == 200, "no border");
    press(FE_BTN_RIGHT);
    CHECK(s->border == FE_BORDER_FULL, "border wraps");
    press(FE_BTN_UP);
    press(FE_BTN_UP);
    press(FE_BTN_LEFT);
    CHECK(s->scale == FE_SCALE_STRETCH, "scale wraps backwards");
    press(FE_BTN_DOWN);
    press(FE_BTN_LEFT);
    CHECK(s->filter == FE_FILTER_SCANLINES, "filter: scanlines");
    press(FE_BTN_A);
    CHECK(s->filter == FE_FILTER_SHARP, "A changes an option (wraps)");
    press(FE_BTN_A);
    CHECK(s->filter == FE_FILTER_NEAREST && read_number("settings.txt", "filter") == FE_FILTER_NEAREST &&
          read_number("settings.txt", "scale") == FE_SCALE_STRETCH, "options saved");
    /* B goes back to the main page, with the cursor on VIDEO */
    press(FE_BTN_B);
    CHECK(fe_menu_open(), "B on a page goes back to the main page");
    press(FE_BTN_A);
    for (int i = 0; i < V_BACK; i++)
        press(FE_BTN_DOWN);
    press(FE_BTN_A); /* BACK line: main page, cursor on VIDEO */
    press(FE_BTN_A); /* VIDEO again */
    press(FE_BTN_DOWN);
    CHECK(fe_menu_open() && s->filter == FE_FILTER_NEAREST, "BACK line and B return to the main page");
    menu_close();
    CHECK(!fe_menu_open(), "menu closed");

    /* sound: volume clamps, sound off is silent */
    menu_goto(M_SOUND, S_VOLUME);
    for (int i = 0; i < 4; i++)
        press(FE_BTN_RIGHT);
    CHECK(s->volume == 10, "volume clamps at 10 (%d)", s->volume);
    for (int i = 0; i < 12; i++)
        press(FE_BTN_LEFT);
    CHECK(s->volume == 1, "volume clamps at 1 (%d)", s->volume);
    press(FE_BTN_RIGHT);
    CHECK(s->volume == 2 && read_number("settings.txt", "volume") == 2, "volume 2 saved");
    press(FE_BTN_UP);
    press(FE_BTN_A);
    CHECK(s->sound == 0, "sound off");
    menu_close();
    audio_samples = 0;
    audio_abs_sum = 0;
    frames(50, 0);
    CHECK(audio_samples > 0 && audio_abs_sum == 0, "sound off gives silence");
    menu_goto(M_SOUND, S_SOUND);
    press(FE_BTN_A);
    CHECK(s->sound == 1, "sound on");
    menu_close();

    /* volume scales the sound: the same 100 frames at volume 10 and 2 */
    {
        long sum[2];
        size_t cnt[2];
        menu_goto(-1, M_SAVE);
        press(FE_BTN_A);
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                menu_goto(-1, M_LOAD);
                press(FE_BTN_A);
            }
            menu_goto(M_SOUND, S_VOLUME);
            for (int i = 0; i < 10; i++)
                press(pass == 0 ? FE_BTN_RIGHT : FE_BTN_LEFT);
            if (pass == 1)
                press(FE_BTN_RIGHT);
            CHECK(s->volume == (pass == 0 ? 10 : 2), "volume %d", s->volume);
            menu_close();
            audio_samples = 0;
            audio_abs_sum = 0;
            frames(100, 0);
            sum[pass] = audio_abs_sum;
            cnt[pass] = audio_samples;
        }
        CHECK(cnt[0] == cnt[1], "same number of samples (%zu, %zu)", cnt[0], cnt[1]);
        CHECK(sum[0] > 0 && sum[1] * 5 <= sum[0] && sum[1] * 5 >= sum[0] - 5 * (long)cnt[0],
              "volume 2 is a fifth of volume 10 (%ld, %ld)", sum[1], sum[0]);
        double per_frame = (double)cnt[0] / 100.0;
        CHECK(per_frame > 950 && per_frame < 966, "about 957.6 samples per frame (%.1f)", per_frame);
    }
}

static void test_controls(void)
{
    const fe_settings_t *s = fe_get_settings();
    /* A -> PAUSE: A presses the P key and does not fire */
    menu_goto(M_CONTROLS, C_MAP0 + FE_MAP_A);
    press(FE_BTN_RIGHT);
    CHECK(s->map[FE_MAP_A] == FE_ACT_PAUSE, "A mapped to pause (%d)", s->map[FE_MAP_A]);
    CHECK(read_number("settings.txt", "map_a") == FE_ACT_PAUSE, "mapping saved");
    menu_close();
    frame(FE_BTN_A);
    CHECK((C.kb_matrix[5] & (1 << 1)) && !(C.joy2 & LJ_JOY_FIRE), "A now pauses, no fire");
    frame(0);
    /* in the menu, A still selects */
    press(FE_BTN_SELECT);
    press(FE_BTN_A);
    CHECK(!fe_menu_open(), "A selects RESUME in the menu, whatever its mapping");

    /* B -> MENU: B opens the menu; in the menu B goes back */
    menu_goto(M_CONTROLS, C_MAP0 + FE_MAP_B);
    for (int i = 0; i < FE_ACT_MENU - FE_ACT_FIRE; i++)
        press(FE_BTN_RIGHT);
    CHECK(s->map[FE_MAP_B] == FE_ACT_MENU, "B mapped to menu");
    menu_close();
    press(FE_BTN_B);
    CHECK(fe_menu_open(), "B opens the menu");
    press(FE_BTN_B);
    CHECK(!fe_menu_open(), "B closes it");

    /* L1 -> QUICK SAVE, R1 -> QUICK LOAD */
    menu_goto(M_CONTROLS, C_MAP0 + FE_MAP_L1);
    press(FE_BTN_RIGHT); /* FAST -> QUICK SAVE */
    press(FE_BTN_DOWN);
    for (int i = 0; i < FE_ACT_LOAD - FE_ACT_MUSIC; i++)
        press(FE_BTN_RIGHT); /* MUSIC -> QUICK LOAD */
    CHECK(s->map[FE_MAP_L1] == FE_ACT_SAVE && s->map[FE_MAP_R1] == FE_ACT_LOAD, "quick save/load mapped");
    menu_close();
    remove_file("state1.bin");
    frame(FE_BTN_L1); /* saves, then this frame runs */
    CHECK(file_exists("state1.bin"), "L1 quick save");
    unsigned fq = lj_frame_count() - 1;
    frame(0);
    frames(60, FE_BTN_RIGHT);
    frame(FE_BTN_R1); /* loads, then this frame runs */
    CHECK(lj_frame_count() == fq + 1, "R1 quick load (%u vs %u)", lj_frame_count(), fq + 1);
    frame(0);

    /* defaults, then auto fire: 3 frames on, 3 off while held */
    menu_goto(M_CONTROLS, C_DEFAULTS);
    press(FE_BTN_A);
    CHECK(s->map[FE_MAP_A] == FE_ACT_FIRE && s->map[FE_MAP_B] == FE_ACT_FIRE &&
          s->map[FE_MAP_L1] == FE_ACT_FAST && s->map[FE_MAP_R1] == FE_ACT_MUSIC, "default controls back");
    press(FE_BTN_UP);
    press(FE_BTN_A);
    CHECK(s->autofire == 1, "auto fire on");
    menu_close();
    int pattern = 0;
    for (int i = 0; i < 12; i++) {
        frame(FE_BTN_A);
        pattern |= ((C.joy2 & LJ_JOY_FIRE) ? 1 : 0) << i;
    }
    frame(0);
    CHECK(pattern == 0x1C7, "auto fire pattern %03x (want 1c7)", pattern);
    menu_goto(M_CONTROLS, C_AUTOFIRE);
    press(FE_BTN_A);
    CHECK(s->autofire == 0, "auto fire off");
    menu_close();
    frames(3, FE_BTN_A);
    CHECK(C.joy2 & LJ_JOY_FIRE, "steady fire again");
    frame(0);

    /* fast forward: hold L1, 3 game frames per frame, one frame of sound */
    unsigned f0 = lj_frame_count();
    audio_samples = 0;
    frames(10, FE_BTN_L1);
    CHECK(lj_frame_count() - f0 == 30, "3x fast forward (%u)", lj_frame_count() - f0);
    CHECK(audio_samples > 9000 && audio_samples < 10200, "sound of one frame per frame (%zu)", audio_samples);
    CHECK(count_color(WHITE, 8, 8, 64, 16) > 10, "fast forward sign drawn");
    menu_goto(M_EXTRAS, E_FFSPEED);
    press(FE_BTN_RIGHT);
    CHECK(s->ff_speed == 4, "4x");
    menu_close();
    f0 = lj_frame_count();
    frames(5, FE_BTN_L2);
    CHECK(lj_frame_count() - f0 == 20, "4x fast forward with L2 (%u)", lj_frame_count() - f0);
    frame(0);
    CHECK(count_color(WHITE, 8, 8, 64, 16) == 0, "sign gone");
}

static void test_lives(void)
{
    const fe_settings_t *s = fe_get_settings();
    /* without the cheat a life is lost within 650 frames of play */
    fresh_start();
    start_game();
    int l0 = lj_lives();
    frames(650, 0);
    CHECK(lj_lives() < l0 && lj_peek(0x0FDF) == 0xCE, "life lost without the cheat (%d -> %d)", l0, lj_lives());

    menu_goto(M_EXTRAS, E_LIVES);
    press(FE_BTN_A);
    CHECK(s->inf_lives == 1, "infinite lives on");
    menu_close();
    /* same play again with the cheat */
    fresh_start();
    start_game();
    l0 = lj_lives();
    frames(650, 0);
    CHECK(lj_peek(0x0FDF) == 0xAD, "patched (DEC -> LDA)");
    CHECK(lj_lives() == l0, "no life lost with the cheat (%d -> %d)", l0, lj_lives());
    frames(2000, 0);
    CHECK(lj_lives() == l0, "still no life lost after 2000 frames");
    /* off again: the code is restored */
    menu_goto(M_EXTRAS, E_LIVES);
    press(FE_BTN_A);
    menu_close();
    CHECK(s->inf_lives == 0 && lj_peek(0x0FDF) == 0xCE, "code restored");
}

static void test_hiscore(void)
{
    const fe_settings_t *s = fe_get_settings();
    remove_file("hiscore.txt");
    fresh_start();
    CHECK(fe_best_score() == 0, "no high score yet");
    start_game();
    set_score(1230);
    /* lose all lives: at game over the game copies the score to HI */
    for (int i = 0; i < 6000 && fe_best_score() == 0; i++)
        frame(0);
    CHECK(lj_hiscore() == 1230, "game's high score 1230 (%u)", lj_hiscore());
    CHECK(fe_best_score() == 1230 && read_number("hiscore.txt", NULL) == 1230,
          "high score saved (%u)", fe_best_score());

    /* next session: the saved high score is put back into the game */
    fresh_start();
    CHECK(fe_best_score() == 1230, "high score read back");
    frames(5, 0);
    CHECK(lj_hiscore() == 1230, "high score in the game after start (%u)", lj_hiscore());
    start_game();
    char hs[7];
    hi_on_screen(hs);
    CHECK(!strcmp(hs, "001230"), "HI 001230 on the screen (%s)", hs);

    /* with infinite lives a score is not saved */
    menu_goto(M_EXTRAS, E_LIVES);
    press(FE_BTN_A);
    menu_close();
    fresh_start();
    start_game();
    set_score(5000);
    menu_goto(M_EXTRAS, E_LIVES);
    press(FE_BTN_A); /* cheat off again: this game is still not counted */
    menu_close();
    CHECK(s->inf_lives == 0, "cheat off");
    for (int i = 0; i < 6000 && lj_hiscore() < 5000; i++)
        frame(0);
    CHECK(lj_hiscore() == 5000, "game's high score 5000 (%u)", lj_hiscore());
    frames(10, 0);
    CHECK(fe_best_score() == 1230 && read_number("hiscore.txt", NULL) == 1230,
          "score of a game with the cheat not saved (%u)", fe_best_score());

    /* a loaded state's high score is not a record */
    menu_goto(-1, M_SAVE);
    press(FE_BTN_A);
    fresh_start();
    menu_goto(-1, M_LOAD);
    press(FE_BTN_A);
    frames(10, 0);
    CHECK(fe_best_score() == 1230, "loaded high score not saved (%u)", fe_best_score());

    /* the game copies the score into HI byte by byte, high byte first; a
     * frame can end in the middle. Copy of 010000 over 001230: for one
     * frame HI reads 011230. That value must never be saved. */
    fresh_start();
    start_game();
    set_score(10000);
    lj_set_hiscore(11230); /* the copy has done the high byte only */
    frame(0);
    lj_set_hiscore(10000); /* the copy is complete */
    frames(5, 0);
    CHECK(fe_best_score() == 10000, "half copied high score not saved (%u)", fe_best_score());

    /* clear the high score: two presses */
    menu_goto(M_EXTRAS, E_HISCORE);
    press(FE_BTN_A);
    CHECK(fe_best_score() == 10000, "first press only asks");
    press(FE_BTN_A);
    CHECK(fe_best_score() == 0 && read_number("hiscore.txt", NULL) == 0, "high score cleared");
    menu_close();
    fresh_start();
    frames(5, 0);
    CHECK(lj_hiscore() == 0, "game starts with HI 0 (%u)", lj_hiscore());
}

static void test_touch_and_session(void)
{
    const fe_settings_t *s = fe_get_settings();
    /* touch: rows of the menu pages (image coordinates) */
    fe_touch_menu();
    CHECK(fe_menu_open(), "touch corner opens the menu");
    CHECK(fe_menu_tap(100, 10) == 0, "tap above the menu does nothing");
    CHECK(fe_menu_tap(100, 40 + 12 * M_SOUND + 3) == 1, "tap SOUND >");
    CHECK(fe_menu_tap(100, 40 + 12 * S_SOUND + 3) == 1 && s->sound == 0, "tap: sound off");
    fe_menu_tap(100, 40 + 12 * S_SOUND + 3);
    CHECK(s->sound == 1, "tap: sound on");
    fe_menu_tap(100, 40 + 12 * S_BACK + 3);
    fe_menu_tap(100, 40 + 12 * M_RESUME + 1);
    CHECK(!fe_menu_open(), "tap RESUME closes the menu");

    /* stick moves the menu cursor once per push: 4 pushes -> VIDEO */
    press(FE_BTN_SELECT);
    for (int k = 0; k < M_VIDEO; k++) {
        in.ay = 0.9f;
        frames(k == 0 ? 30 : 1, 0); /* held long: still one step */
        in.ay = 0.0f;
        frame(0);
    }
    press(FE_BTN_A);
    press(FE_BTN_DOWN);
    int f = s->filter;
    press(FE_BTN_A);
    CHECK(s->filter == (f + 1) % FE_FILTER_COUNT, "stick reached VIDEO, A changed the filter");
    press(FE_BTN_LEFT);
    menu_close();

    /* autosave on pause, resume on the next start */
    frames(77, FE_BTN_RIGHT);
    frame(0);
    fe_pause();
    CHECK(file_exists("autosave.bin"), "autosave.bin written");
    uint32_t h_pause = lj_debug_hash();
    unsigned fc_pause = lj_frame_count();
    CHECK(fe_init(dir, 48000) == 0, "second start");
    CHECK(lj_frame_count() == fc_pause && lj_debug_hash() == h_pause, "resumed where it stopped");

    /* resume off: fresh start */
    menu_goto(M_EXTRAS, E_AUTORESUME);
    press(FE_BTN_A);
    CHECK(s->autoresume == 0, "resume on start off");
    menu_close();
    fe_pause();
    CHECK(fe_init(dir, 48000) == 0, "third start");
    CHECK(lj_frame_count() < 5, "fresh start without resume (%u)", lj_frame_count());
    menu_goto(M_EXTRAS, E_AUTORESUME);
    press(FE_BTN_A);
    menu_close();

    /* quit saves and asks the app to close */
    frames(20, 0);
    menu_goto(-1, M_QUIT);
    remove_file("autosave.bin");
    press(FE_BTN_A);
    CHECK(fe_quit_requested(), "quit requested");
    CHECK(file_exists("autosave.bin"), "quit writes the autosave");

    /* an old settings file: "bfire 0" means B opens the menu */
    write_file("settings.txt", "scale 1\nfilter 2\nborder 0\nsound 1\nvolume 7\nautoresume 1\nbfire 0\n");
    fresh_start();
    CHECK(s->scale == 1 && s->filter == 2 && s->volume == 7 && s->map[FE_MAP_B] == FE_ACT_MENU &&
          s->map[FE_MAP_A] == FE_ACT_FIRE, "old settings file read");
    /* bad values are clamped */
    write_file("settings.txt", "scale 99\nvolume -5\nslot 9\nffspeed 1\nmap_x 42\n");
    fresh_start();
    CHECK(s->scale == FE_SCALE_COUNT - 1 && s->volume == 1 && s->slot == FE_SLOTS && s->ff_speed == 2 &&
          s->map[FE_MAP_X] == FE_ACT_COUNT - 1, "bad values clamped");
    remove_file("settings.txt");

    /* determinism: the same input gives the same machine state */
    fresh_start();
    for (int i = 0; i < 600; i++)
        frame(i % 50 < 10 ? FE_BTN_A : (i % 97 < 40 ? FE_BTN_RIGHT : 0));
    uint32_t ha = lj_debug_hash();
    fresh_start();
    for (int i = 0; i < 600; i++)
        frame(i % 50 < 10 ? FE_BTN_A : (i % 97 < 40 ? FE_BTN_RIGHT : 0));
    CHECK(lj_debug_hash() == ha, "same input, same state");
}
#endif

#ifdef LJ_NOGAME
static void test_nogame(void)
{
    CHECK(fe_init(dir, 48000) != 0, "fe_init must fail without the game");
    frame(0);
    CHECK(count_color(WHITE, 0, 0, FE_W, FE_H) > 100, "message text is drawn");
    CHECK(!fe_quit_requested(), "no quit yet");
    CHECK(!lj_extras_available(), "no game hooks");
    press(FE_BTN_MENU);
    CHECK(fe_quit_requested(), "Back quits the message screen");
    int16_t buf[64];
    CHECK(fe_audio_read(buf, 64) == 0, "no sound without the game");
}
#endif

int main(int argc, char **argv)
{
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "fetest.dir");
    mkdir(dir, 0755);
#ifdef LJ_NOGAME
    test_nogame();
#else
    test_basics();
    test_states();
    test_settings_pages();
    test_controls();
    test_lives();
    test_hiscore();
    test_touch_and_session();
#endif
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
