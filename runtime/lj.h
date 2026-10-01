/*
 * lj.h - public API of the Lazy Jones runtime, used by the platform front
 * ends (Android, host test tools).
 *
 * Typical use:
 *   lj_game_init(48000);          power on, load the game, start it
 *   each frame (50.125 Hz):
 *     lj_set_joystick(2, bits);   input
 *     lj_run_frame();             emulate one PAL frame
 *     lj_framebuffer();           384x272 palette indices (0..15)
 *     lj_audio_read(buf, n);      16 bit mono samples
 */
#ifndef LJ_H
#define LJ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LJ_FB_W 384
#define LJ_FB_H 272
#define LJ_FRAME_HZ (985248.0 / 19656.0) /* 50.1245 */

/* joystick bits (active high) */
#define LJ_JOY_UP 0x01
#define LJ_JOY_DOWN 0x02
#define LJ_JOY_LEFT 0x04
#define LJ_JOY_RIGHT 0x08
#define LJ_JOY_FIRE 0x10

/* keyboard matrix positions: (column = CIA1 PA bit, row = CIA1 PB bit) */
#define LJ_KEY(col, row) (((col) << 3) | (row))
#define LJ_KEY_RETURN LJ_KEY(0, 1)
#define LJ_KEY_F1 LJ_KEY(0, 4)
#define LJ_KEY_F3 LJ_KEY(0, 5)
#define LJ_KEY_F5 LJ_KEY(0, 6)
#define LJ_KEY_F7 LJ_KEY(0, 3)
#define LJ_KEY_LSHIFT LJ_KEY(1, 7)
#define LJ_KEY_RSHIFT LJ_KEY(6, 4)
#define LJ_KEY_EQUALS LJ_KEY(6, 5)
#define LJ_KEY_UPARROW LJ_KEY(6, 6)
#define LJ_KEY_SPACE LJ_KEY(7, 4)
#define LJ_KEY_CBM LJ_KEY(7, 5)
#define LJ_KEY_RUNSTOP LJ_KEY(7, 7)
#define LJ_KEY_Q LJ_KEY(7, 6)
#define LJ_KEY_1 LJ_KEY(7, 0)
#define LJ_KEY_2 LJ_KEY(7, 3)
#define LJ_KEY_3 LJ_KEY(1, 0)
#define LJ_KEY_4 LJ_KEY(1, 3)
#define LJ_KEY_5 LJ_KEY(2, 0)
#define LJ_KEY_6 LJ_KEY(2, 3)
#define LJ_KEY_7 LJ_KEY(3, 0)
#define LJ_KEY_8 LJ_KEY(3, 3)
#define LJ_KEY_9 LJ_KEY(4, 0)
#define LJ_KEY_0 LJ_KEY(4, 3)
#define LJ_KEY_Y LJ_KEY(3, 1)
#define LJ_KEY_N LJ_KEY(4, 7)

typedef void (*lj_log_fn)(const char *msg);
void lj_set_log(lj_log_fn fn);

/* machine level */
void lj_power_on(int sample_rate);
int lj_load_prg(const uint8_t *data, size_t len);
void lj_start(uint16_t entry);
void lj_run_frame(void);
const uint8_t *lj_framebuffer(void);
void lj_set_joystick(int port, uint8_t bits);
void lj_set_key(int col, int row, int down);
void lj_clear_keys(void);
size_t lj_audio_read(int16_t *out, size_t max);
void lj_set_sample_rate(double rate);
size_t lj_state_size(void);
void lj_state_save(void *buf);
int lj_state_load(const void *buf, size_t len);
uint64_t lj_clock(void);
uint32_t lj_frame_count(void);

/* game level (lj_game.c + generated data): power on, load the embedded game
 * image, register the recompiled code and start the game. Returns 0 on
 * success. */
int lj_game_init(int sample_rate);
const char *lj_game_version(void);

/* Enhancements (lj_extras.c). They work only when the loaded game code is
 * the known code (lj_extras_available() == 1); otherwise they do nothing. */
int lj_extras_available(void);
void lj_cheat_infinite_lives(int on); /* call before each lj_run_frame */
uint32_t lj_score(void);              /* current score */
uint32_t lj_hiscore(void);            /* high score in the game's memory */
void lj_set_hiscore(uint32_t v);      /* 0..999999 */
int lj_lives(void);                   /* lives left, -1 when not available */
/* RAM access that keeps the recompiled code correct when code is changed */
void lj_poke(uint16_t addr, uint8_t v);
uint8_t lj_peek(uint16_t addr);

/* palette: 16 colours as 0xRRGGBB */
extern const uint32_t lj_palette[16];

#ifdef __cplusplus
}
#endif

#endif
