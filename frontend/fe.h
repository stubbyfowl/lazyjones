/*
 * fe.h - portable front end: menu, settings, save states, input mapping,
 * enhancements and frame composition. Platform layers (Android) feed input,
 * call fe_frame() at 50.125 Hz, display fe_rgba() and play fe_audio_read().
 */
#ifndef LJ_FE_H
#define LJ_FE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* physical buttons (what the platform layer reports) */
enum {
    FE_BTN_UP = 1 << 0,
    FE_BTN_DOWN = 1 << 1,
    FE_BTN_LEFT = 1 << 2,
    FE_BTN_RIGHT = 1 << 3,
    FE_BTN_A = 1 << 4,
    FE_BTN_B = 1 << 5,
    FE_BTN_X = 1 << 6,
    FE_BTN_Y = 1 << 7,
    FE_BTN_L1 = 1 << 8,
    FE_BTN_R1 = 1 << 9,
    FE_BTN_L2 = 1 << 10,
    FE_BTN_R2 = 1 << 11,
    FE_BTN_START = 1 << 12,
    FE_BTN_SELECT = 1 << 13,
    FE_BTN_MENU = 1 << 14,   /* Back, Menu, Esc: always opens the menu */
    FE_BTN_TOUCH_FIRE = 1 << 15, /* fire area of the touch screen: always fire */
};

/* the buttons that can be mapped, in this order */
enum {
    FE_MAP_A, FE_MAP_B, FE_MAP_X, FE_MAP_Y, FE_MAP_L1, FE_MAP_R1,
    FE_MAP_L2, FE_MAP_R2, FE_MAP_START, FE_MAP_SELECT, FE_MAP_COUNT
};

/* what a mapped button does */
enum {
    FE_ACT_NONE,
    FE_ACT_FIRE,    /* joystick fire */
    FE_ACT_PAUSE,   /* C64 key P: the game's pause */
    FE_ACT_MUSIC,   /* C64 key M: the game's music on/off */
    FE_ACT_MENU,    /* this menu */
    FE_ACT_FAST,    /* fast forward while held */
    FE_ACT_SAVE,    /* quick save to the current slot */
    FE_ACT_LOAD,    /* quick load from the current slot */
    FE_ACT_COUNT
};

typedef struct {
    uint32_t held;   /* FE_BTN_* currently pressed */
    float ax, ay;    /* analog stick, -1..1 (y down positive) */
} fe_input_t;

enum { FE_SCALE_FIT = 0, FE_SCALE_INTEGER, FE_SCALE_STRETCH, FE_SCALE_COUNT };
enum { FE_FILTER_SHARP = 0, FE_FILTER_NEAREST, FE_FILTER_SMOOTH, FE_FILTER_SCANLINES,
       FE_FILTER_COUNT };
enum { FE_BORDER_FULL = 0, FE_BORDER_SMALL, FE_BORDER_NONE, FE_BORDER_COUNT };

#define FE_SLOTS 4

typedef struct {
    int scale;      /* FE_SCALE_* */
    int filter;     /* FE_FILTER_* */
    int border;     /* FE_BORDER_* */
    int sound;      /* 0 off, 1 on */
    int volume;     /* 1..10 */
    int autoresume; /* resume the last session on start */
    int map[FE_MAP_COUNT]; /* FE_ACT_* for each mappable button */
    int autofire;   /* fire repeats while held */
    int slot;       /* save state slot 1..FE_SLOTS */
    int inf_lives;  /* cheat: lives are not used up */
    int ff_speed;   /* fast forward speed 2..4 */
} fe_settings_t;

#define FE_W 384
#define FE_H 272
/* pixel aspect ratio of a PAL C64 (width / height of one pixel) */
#define FE_PAR 0.9365f

int fe_init(const char *data_dir, int sample_rate);
void fe_set_sample_rate(double rate);  /* fine adjustment (rate control) */
void fe_set_output_rate(int rate);     /* the audio device's rate */
void fe_set_input(const fe_input_t *in);
void fe_touch_menu(void); /* open the menu (touch corner) */
/* Run one frame (game or menu). Returns 1 when the game advanced. */
int fe_frame(void);
const uint32_t *fe_rgba(void);         /* FE_W x FE_H, 0xAABBGGRR bytes R,G,B,A */
void fe_view(int *x, int *y, int *w, int *h); /* visible part of the image */
const fe_settings_t *fe_get_settings(void);
size_t fe_audio_read(int16_t *out, size_t max);
int fe_audio_wanted(void);             /* 1 when sound should play */
void fe_pause(void);                   /* app goes to background: save */
int fe_quit_requested(void);
int fe_menu_open(void);
/* touch support: map a tap at (x, y) in image coordinates while the menu
 * is open; returns 1 if it hit a menu line */
int fe_menu_tap(int x, int y);
uint32_t fe_best_score(void);          /* saved high score */

#ifdef __cplusplus
}
#endif

#endif
