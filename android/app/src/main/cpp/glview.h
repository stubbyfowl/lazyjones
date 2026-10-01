/*
 * glview.h - draws the game picture with OpenGL ES 2.0. Used by the app
 * (main.c) and by the host test (host/gltest.c).
 */
#ifndef LJ_GLVIEW_H
#define LJ_GLVIEW_H

#include <stdint.h>
#include "fe.h"

/* Where shader compile errors go (logcat on Android). Default: nowhere. */
void glview_set_log(void (*fn)(const char *msg));

/* Make the programs and the texture. A GL ES 2 context must be current.
 * Returns 1 on success. */
int glview_init(void);
/* Forget the GL objects (they died with the context). */
void glview_forget(void);
int glview_ready(void);
/* 1 when the high precision shaders compiled (sharp and scanline filters) */
int glview_has_sharp(void);

/* Where the picture goes in a window of ww x wh pixels (top left origin):
 * source rectangle sw x sh, settings for scale mode. */
void glview_dest(int ww, int wh, int sw, int sh, const fe_settings_t *s,
                 float *x, float *y, float *w, float *h);

/* Clear the window and draw the part (sx, sy, sw, sh) of the FE_W x FE_H
 * RGBA image into the rectangle (dx, dy, dw, dh). */
void glview_draw(int ww, int wh, const uint32_t *rgba, int sx, int sy, int sw, int sh,
                 const fe_settings_t *s, float dx, float dy, float dw, float dh);

#endif
