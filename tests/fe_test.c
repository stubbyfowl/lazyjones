/*
 * fe_test.c - tests of the front end (frontend/fe.c) on the host: menu,
 * save states, settings, autosave, input mapping and sound volume.
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

static int count_color(uint32_t c)
{
    const uint32_t *px = fe_rgba();
    int n = 0;
    for (int i = 0; i < FE_W * FE_H; i++)
        n += px[i] == c;
    return n;
}

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

static int read_setting(const char *key)
{
    char p[512], k[32];
    int v, found = -1;
    snprintf(p, sizeof p, "%s/settings.txt", dir);
    FILE *f = fopen(p, "r");
    if (!f)
        return -1;
    while (fscanf(f, "%31s %d", k, &v) == 2)
        if (!strcmp(k, key))
            found = v;
    fclose(f);
    return found;
}

/* open the menu (the cursor starts on row 0) and move to row 'item' */
static void menu_goto(int item)
{
    if (fe_menu_open())
        press(FE_BTN_FIRE2); /* B always closes the menu */
    press(FE_BTN_SELECT);
    for (int i = 0; i < item; i++)
        press(FE_BTN_DOWN);
}

enum { R_RESUME, R_SAVE, R_LOAD, R_RESET, R_SCALE, R_FILTER, R_BORDER,
       R_SOUND, R_VOLUME, R_BBUTTON, R_AUTORESUME, R_QUIT };
#endif

#ifdef LJ_NOGAME
static void test_nogame(void)
{
    CHECK(fe_init(dir, 48000) != 0, "fe_init must fail without the game");
    frame(0);
    CHECK(count_color(0xFFFFFFFFu) > 100, "message text is drawn");
    CHECK(!fe_quit_requested(), "no quit yet");
    press(FE_BTN_MENU);
    CHECK(fe_quit_requested(), "Back quits the message screen");
    int16_t buf[64];
    CHECK(fe_audio_read(buf, 64) == 0, "no sound without the game");
}
#else
static void test_game(void)
{
    remove_file("settings.txt");
    remove_file("autosave.bin");
    remove_file("state1.bin");

    CHECK(fe_init(dir, 48000) == 0, "fe_init with the game");
    const fe_settings_t *s = fe_get_settings();
    CHECK(s->scale == FE_SCALE_FIT && s->filter == FE_FILTER_SHARP && s->border == FE_BORDER_FULL &&
          s->sound == 1 && s->volume == 8 && s->autoresume == 1 && s->bfire == 1, "default settings");
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

    /* view rectangles */
    int x, y, w, h;
    fe_view(&x, &y, &w, &h);
    CHECK(x == 0 && y == 0 && w == FE_W && h == FE_H, "full border view");

    /* joystick mapping (port 2, active high bits) */
    frame(FE_BTN_LEFT | FE_BTN_FIRE);
    CHECK(C.joy2 == (LJ_JOY_LEFT | LJ_JOY_FIRE), "left+fire -> %02x", C.joy2);
    frame(FE_BTN_LEFT | FE_BTN_RIGHT | FE_BTN_UP);
    CHECK(C.joy2 == LJ_JOY_UP, "opposite directions cancel -> %02x", C.joy2);
    in.ax = 0.3f; in.ay = -0.9f;
    frame(0);
    CHECK(C.joy2 == LJ_JOY_UP, "stick: dead zone x, up y -> %02x", C.joy2);
    in.ax = 0.0f; in.ay = 0.0f;
    frame(FE_BTN_FIRE2);
    CHECK(C.joy2 == LJ_JOY_FIRE, "B is fire by default -> %02x", C.joy2);
    frame(FE_BTN_FIRE3);
    CHECK(C.joy2 == LJ_JOY_FIRE, "X/Y is fire -> %02x", C.joy2);
    frame(0);
    CHECK(C.joy2 == 0, "released -> %02x", C.joy2);

    /* START holds the P key (column 5, row 1), R1 the M key (column 4, row 4) */
    frame(FE_BTN_START);
    CHECK(C.kb_matrix[5] & (1 << 1), "START presses P");
    frame(FE_BTN_R);
    CHECK(!(C.kb_matrix[5] & (1 << 1)) && (C.kb_matrix[4] & (1 << 4)), "R releases P, presses M");
    frame(0);
    CHECK(C.kb_matrix[4] == 0 && C.kb_matrix[5] == 0, "keys released");

    /* menu: opens with SELECT, game stops, sound muted */
    press(FE_BTN_SELECT);
    CHECK(fe_menu_open(), "SELECT opens the menu");
    f0 = lj_frame_count();
    audio_samples = 0;
    frames(20, 0);
    CHECK(lj_frame_count() == f0, "game paused in the menu");
    CHECK(audio_samples == 0, "no sound in the menu");
    CHECK(count_color(0xFFFFFFFFu) > 50, "menu text drawn");
    frame(FE_BTN_FIRE | FE_BTN_LEFT);
    CHECK(C.joy2 == 0, "no joystick input reaches the game in the menu");
    frame(0);
    press(FE_BTN_FIRE2);
    CHECK(!fe_menu_open(), "B closes the menu");

    /* START held while the menu opens and closes: P is pressed again */
    frame(FE_BTN_START);
    frame(FE_BTN_START | FE_BTN_SELECT);
    CHECK(fe_menu_open(), "menu opened with START held");
    CHECK(!(C.kb_matrix[5] & (1 << 1)), "P released in the menu");
    frame(FE_BTN_START | FE_BTN_FIRE2);
    CHECK(!fe_menu_open(), "menu closed with START held");
    frame(FE_BTN_START);
    CHECK(C.kb_matrix[5] & (1 << 1), "P pressed again after the menu");
    frame(0);
    frame(0);

    /* save state */
    menu_goto(R_SAVE);
    press(FE_BTN_FIRE);
    CHECK(!fe_menu_open(), "menu closes after save");
    CHECK(file_exists("state1.bin"), "state1.bin written");
    uint32_t h_saved = lj_debug_hash();
    unsigned fc_saved = lj_frame_count();
    frames(300, FE_BTN_RIGHT);
    frames(100, FE_BTN_FIRE);
    CHECK(lj_debug_hash() != h_saved, "state changed after 400 frames");

    /* load state */
    menu_goto(R_LOAD);
    press(FE_BTN_FIRE);
    CHECK(!fe_menu_open(), "menu closes after load");
    CHECK(lj_frame_count() == fc_saved, "frame counter restored");
    /* fe_frame drew the menu and toast into the RGBA image only; the
     * machine state must be exactly the saved one */
    CHECK(lj_debug_hash() == h_saved, "machine state restored (%08x vs %08x)",
          lj_debug_hash(), h_saved);

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
        unsigned fc_now = lj_frame_count();
        menu_goto(R_LOAD);
        press(FE_BTN_FIRE); /* the release frame runs one game frame */
        CHECK(lj_frame_count() == fc_now + 1, "bad state file not loaded (%u, %u)",
              lj_frame_count(), fc_now);
    }

    /* reset needs two presses */
    frames(50, 0);
    menu_goto(R_RESET);
    press(FE_BTN_FIRE);
    CHECK(fe_menu_open(), "first press only asks");
    unsigned fc = lj_frame_count();
    press(FE_BTN_FIRE);
    CHECK(!fe_menu_open(), "second press resets");
    CHECK(lj_frame_count() < fc, "frame counter restarted (%u < %u)", lj_frame_count(), fc);
    menu_goto(R_RESET);
    press(FE_BTN_FIRE);
    press(FE_BTN_DOWN);
    press(FE_BTN_UP);
    press(FE_BTN_FIRE);
    CHECK(fe_menu_open(), "moving away cancels the reset question");
    press(FE_BTN_FIRE2);

    /* settings change and are saved */
    menu_goto(R_VOLUME);
    press(FE_BTN_RIGHT);
    press(FE_BTN_RIGHT);
    press(FE_BTN_RIGHT);
    CHECK(s->volume == 10, "volume clamps at 10 (%d)", s->volume);
    CHECK(read_setting("volume") == 10, "volume saved");
    for (int i = 0; i < 12; i++)
        press(FE_BTN_LEFT);
    CHECK(s->volume == 1, "volume clamps at 1 (%d)", s->volume);
    press(FE_BTN_RIGHT);
    CHECK(s->volume == 2, "volume 2");
    menu_goto(R_BORDER);
    press(FE_BTN_RIGHT);
    CHECK(s->border == FE_BORDER_SMALL, "border small");
    fe_view(&x, &y, &w, &h);
    CHECK(x == 16 && y == 19 && w == 352 && h == 232, "small border view");
    press(FE_BTN_RIGHT);
    fe_view(&x, &y, &w, &h);
    CHECK(s->border == FE_BORDER_NONE && x == 32 && y == 35 && w == 320 && h == 200, "no border view");
    press(FE_BTN_RIGHT);
    CHECK(s->border == FE_BORDER_FULL, "border wraps");
    menu_goto(R_SCALE);
    press(FE_BTN_LEFT);
    CHECK(s->scale == FE_SCALE_STRETCH, "scale wraps backwards");
    menu_goto(R_FILTER);
    press(FE_BTN_FIRE);
    CHECK(s->filter == FE_FILTER_NEAREST, "A changes an option");
    CHECK(read_setting("filter") == FE_FILTER_NEAREST && read_setting("scale") == FE_SCALE_STRETCH,
          "options saved");
    press(FE_BTN_FIRE2);

    /* volume scales the sound: the same 100 frames (from a saved state)
     * at volume 10 and at volume 2 */
    {
        long sum[2];
        size_t cnt[2];
        menu_goto(R_SAVE);
        press(FE_BTN_FIRE);
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                menu_goto(R_LOAD);
                press(FE_BTN_FIRE);
            }
            menu_goto(R_VOLUME);
            for (int i = 0; i < 10; i++)
                press(pass == 0 ? FE_BTN_RIGHT : FE_BTN_LEFT);
            if (pass == 1)
                press(FE_BTN_RIGHT); /* 1 -> 2 */
            CHECK(s->volume == (pass == 0 ? 10 : 2), "volume %d", s->volume);
            press(FE_BTN_FIRE2);
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

    /* B button as menu key */
    menu_goto(R_BBUTTON);
    press(FE_BTN_FIRE);
    CHECK(s->bfire == 0, "B set to menu");
    press(FE_BTN_FIRE2);
    CHECK(!fe_menu_open(), "B closes the menu");
    press(FE_BTN_FIRE2);
    CHECK(fe_menu_open(), "B opens the menu now");
    frame(0);
    menu_goto(R_BBUTTON);
    press(FE_BTN_FIRE);
    CHECK(s->bfire == 1, "B set to fire");
    press(FE_BTN_FIRE2);

    /* touch: tap rows of the menu (image coordinates) */
    fe_touch_menu();
    CHECK(fe_menu_open(), "touch corner opens the menu");
    CHECK(fe_menu_tap(100, 10) == 0, "tap above the menu does nothing");
    CHECK(fe_menu_tap(100, 44 + 12 * R_SOUND + 3) == 1, "tap on SOUND");
    CHECK(s->sound == 0, "sound off by tap");
    audio_samples = 0;
    audio_abs_sum = 0;
    press(FE_BTN_FIRE2);
    frames(50, 0);
    CHECK(audio_samples > 0 && audio_abs_sum == 0, "sound off gives silence");
    fe_touch_menu();
    fe_menu_tap(100, 44 + 12 * R_SOUND + 3);
    CHECK(s->sound == 1, "sound on again");
    fe_menu_tap(100, 44 + 12 * R_RESUME + 1);
    CHECK(!fe_menu_open(), "tap RESUME closes the menu");

    /* stick moves the menu cursor once per push: 7 pushes -> SOUND */
    press(FE_BTN_SELECT);
    for (int k = 0; k < R_SOUND; k++) {
        in.ay = 0.9f;
        frames(k == 0 ? 30 : 1, 0); /* held long: still one step */
        in.ay = 0.0f;
        frame(0);
    }
    press(FE_BTN_FIRE);
    CHECK(s->sound == 0 && fe_menu_open(), "stick reached SOUND, A switched it off");
    press(FE_BTN_FIRE);
    CHECK(s->sound == 1, "sound on again");
    press(FE_BTN_FIRE2);

    /* autosave on pause, resume on the next start */
    frames(77, FE_BTN_RIGHT);
    frame(0);
    fe_pause();
    CHECK(file_exists("autosave.bin"), "autosave.bin written");
    uint32_t h_pause = lj_debug_hash();
    unsigned fc_pause = lj_frame_count();
    CHECK(fe_init(dir, 48000) == 0, "second start");
    CHECK(lj_frame_count() == fc_pause && lj_debug_hash() == h_pause, "resumed where it stopped");
    CHECK(s->volume == 2 && s->filter == FE_FILTER_NEAREST, "settings kept");

    /* autoresume off: fresh start */
    menu_goto(R_AUTORESUME);
    press(FE_BTN_FIRE);
    CHECK(s->autoresume == 0, "resume on start off");
    press(FE_BTN_FIRE2);
    fe_pause();
    CHECK(fe_init(dir, 48000) == 0, "third start");
    CHECK(lj_frame_count() < 5, "fresh start without resume (%u)", lj_frame_count());

    /* quit saves and asks the app to close */
    frames(20, 0);
    menu_goto(R_QUIT);
    remove_file("autosave.bin");
    press(FE_BTN_FIRE);
    CHECK(fe_quit_requested(), "quit requested");
    CHECK(file_exists("autosave.bin"), "quit writes the autosave");

    /* determinism: the same input gives the same machine state */
    {
        remove_file("autosave.bin");
        CHECK(fe_init(dir, 48000) == 0, "start A");
        for (int i = 0; i < 600; i++)
            frame(i % 50 < 10 ? FE_BTN_FIRE : (i % 97 < 40 ? FE_BTN_RIGHT : 0));
        uint32_t ha = lj_debug_hash();
        CHECK(fe_init(dir, 48000) == 0, "start B");
        for (int i = 0; i < 600; i++)
            frame(i % 50 < 10 ? FE_BTN_FIRE : (i % 97 < 40 ? FE_BTN_RIGHT : 0));
        CHECK(lj_debug_hash() == ha, "same input, same state");
    }
}
#endif

int main(int argc, char **argv)
{
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : "fetest.dir");
    mkdir(dir, 0755);
#ifdef LJ_NOGAME
    test_nogame();
#else
    test_game();
#endif
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
