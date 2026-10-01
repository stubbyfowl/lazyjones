/*
 * gltest.c - tests the OpenGL ES 2 drawing of the app (glview.c) on the
 * host with Mesa (llvmpipe, no window): the shaders compile, the picture is
 * placed and scaled correctly, and every filter gives the right pixels.
 *
 *   make -C host gltest && host/gltest
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include "glview.h"

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

static uint32_t src[FE_W * FE_H];  /* bytes R, G, B, A in memory */
static uint8_t out[2000 * 1200 * 4];

static uint32_t rgba(int r, int g, int b) { return 0xFF000000u | (uint32_t)(b << 16) | (uint32_t)(g << 8) | (uint32_t)r; }

/* every pixel has its own colour, no channel below 16 (black = background) */
static void pattern(void)
{
    for (int y = 0; y < FE_H; y++)
        for (int x = 0; x < FE_W; x++)
            src[y * FE_W + x] = rgba(16 + (x * 7) % 240, 16 + (y * 13) % 240, 16 + ((x / 3 + y / 5) * 29) % 240);
}

static void solid(uint32_t c)
{
    for (int i = 0; i < FE_W * FE_H; i++)
        src[i] = c;
}

static GLuint fbo, fbo_tex;
static int win_w, win_h;

static void window(int w, int h)
{
    win_w = w;
    win_h = h;
    if (fbo) {
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &fbo_tex);
    }
    glGenTextures(1, &fbo_tex);
    glBindTexture(GL_TEXTURE_2D, fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo_tex, 0);
}

/* draw like the app does and read the window back (row 0 = top) */
static void draw(const fe_settings_t *s, int sx, int sy, int sw, int sh, float *d)
{
    glview_dest(win_w, win_h, sw, sh, s, &d[0], &d[1], &d[2], &d[3]);
    glview_draw(win_w, win_h, src, sx, sy, sw, sh, s, d[0], d[1], d[2], d[3]);
    static uint8_t tmp[2000 * 1200 * 4];
    glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
    for (int y = 0; y < win_h; y++)
        memcpy(&out[(size_t)y * win_w * 4], &tmp[(size_t)(win_h - 1 - y) * win_w * 4], (size_t)win_w * 4);
    CHECK(glGetError() == GL_NO_ERROR, "GL error");
}

static const uint8_t *px(int x, int y) { return &out[((size_t)y * win_w + x) * 4]; }

static int same(const uint8_t *p, uint32_t c, int tol)
{
    for (int i = 0; i < 3; i++)
        if (abs((int)p[i] - (int)((c >> (8 * i)) & 255)) > tol)
            return 0;
    return 1;
}

static fe_settings_t settings(int scale, int filter, int border)
{
    fe_settings_t s;
    memset(&s, 0, sizeof s);
    s.scale = scale;
    s.filter = filter;
    s.border = border;
    return s;
}

/* integer scale k: each source pixel becomes a k x k block, exactly */
static void test_integer(int filter, int k, const char *name)
{
    pattern();
    window(FE_W * k, FE_H * k);
    fe_settings_t s = settings(FE_SCALE_INTEGER, filter, FE_BORDER_FULL);
    float d[4];
    draw(&s, 0, 0, FE_W, FE_H, d);
    CHECK(d[0] == 0 && d[1] == 0 && d[2] == FE_W * k && d[3] == FE_H * k, "%s: dest", name);
    long bad = 0;
    for (int y = 0; y < win_h; y++)
        for (int x = 0; x < win_w; x++)
            bad += !same(px(x, y), src[(y / k) * FE_W + x / k], 0);
    CHECK(bad == 0, "%s %dx: %ld pixels differ from the source", name, k, bad);
}

static void log_msg(const char *msg) { printf("%s\n", msg); }

int main(void)
{
    glview_set_log(log_msg);
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)
                                 : EGL_NO_DISPLAY;
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        printf("no EGL display (Mesa surfaceless platform needed)\n");
        return 2;
    }
    const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n);
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, n ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        printf("no GL ES 2 context\n");
        return 2;
    }
    printf("GL: %s, %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));

    CHECK(glview_init(), "glview_init");
    CHECK(glview_has_sharp(), "sharp and scanline shaders compile");

    /* exact pixel blocks at integer scales */
    test_integer(FE_FILTER_NEAREST, 3, "nearest");
    test_integer(FE_FILTER_SHARP, 3, "sharp");
    test_integer(FE_FILTER_SHARP, 2, "sharp");
    test_integer(FE_FILTER_NEAREST, 1, "nearest");

    /* sharp at a non-integer scale: pixels that do not touch a texel edge
     * are exactly the source pixel; the others are a blend of the two
     * neighbours (each channel between them) */
    {
        pattern();
        window(1000, 720);
        fe_settings_t s = settings(FE_SCALE_STRETCH, FE_FILTER_SHARP, FE_BORDER_FULL);
        float d[4];
        draw(&s, 0, 0, FE_W, FE_H, d);
        float kx = d[2] / FE_W, ky = d[3] / FE_H;
        long inner = 0, bad_inner = 0, bad_edge = 0;
        for (int y = 0; y < win_h; y++)
            for (int x = 0; x < win_w; x++) {
                float tx = ((float)x + 0.5f - d[0]) / kx, ty = ((float)y + 0.5f - d[1]) / ky;
                int ix = (int)floorf(tx), iy = (int)floorf(ty);
                float fx = tx - (float)ix, fy = ty - (float)iy;
                float mx = 0.5f / kx + 0.01f, my = 0.5f / ky + 0.01f;
                const uint8_t *p = px(x, y);
                if (fx > mx && fx < 1 - mx && fy > my && fy < 1 - my) {
                    inner++;
                    bad_inner += !same(p, src[iy * FE_W + ix], 1);
                } else {
                    /* between the 2x2 neighbours */
                    int x0 = fx < 0.5f ? ix - 1 : ix, y0 = fy < 0.5f ? iy - 1 : iy;
                    if (x0 < 0) x0 = 0;
                    if (y0 < 0) y0 = 0;
                    int x1 = x0 + 1 < FE_W ? x0 + 1 : x0, y1 = y0 + 1 < FE_H ? y0 + 1 : y0;
                    for (int c = 0; c < 3; c++) {
                        int lo = 255, hi = 0;
                        uint32_t q[4] = {src[y0 * FE_W + x0], src[y0 * FE_W + x1], src[y1 * FE_W + x0],
                                         src[y1 * FE_W + x1]};
                        for (int i = 0; i < 4; i++) {
                            int v = (int)((q[i] >> (8 * c)) & 255);
                            if (v < lo) lo = v;
                            if (v > hi) hi = v;
                        }
                        if (p[c] + 1 < lo || p[c] > hi + 1)
                            bad_edge++;
                    }
                }
            }
        /* inner share per axis is 1 - 2 * (0.5 / scale + 0.01): about 36 % here */
        CHECK(inner > 240000 && bad_inner == 0, "sharp 2.6x: %ld of %ld inner pixels wrong", bad_inner, inner);
        CHECK(bad_edge == 0, "sharp 2.6x: %ld edge values outside their neighbours", bad_edge);
    }

    /* fit: PAL pixel aspect, centred, black bars */
    {
        pattern();
        window(1600, 900);
        fe_settings_t s = settings(FE_SCALE_FIT, FE_FILTER_SHARP, FE_BORDER_FULL);
        float d[4];
        draw(&s, 0, 0, FE_W, FE_H, d);
        float want_w = floorf(900.0f * FE_W * FE_PAR / FE_H + 0.5f);
        CHECK(d[1] == 0 && d[3] == 900 && d[2] == want_w && d[0] == floorf((1600 - want_w) / 2),
              "fit dest %.0f %.0f %.0f %.0f", d[0], d[1], d[2], d[3]);
        long bar = 0, pic = 0;
        for (int y = 0; y < win_h; y += 7)
            for (int x = 0; x < win_w; x += 3) {
                const uint8_t *p = px(x, y);
                int black = p[0] == 0 && p[1] == 0 && p[2] == 0;
                if (x < d[0] || x >= d[0] + d[2])
                    bar += !black;
                else
                    pic += black;
            }
        CHECK(bar == 0, "bars are black (%ld not)", bar);
        CHECK(pic == 0, "picture has no black pixels (%ld)", pic);
    }

    /* no border: the 320 x 200 C64 screen, 3x */
    {
        pattern();
        window(960, 600);
        fe_settings_t s = settings(FE_SCALE_INTEGER, FE_FILTER_NEAREST, FE_BORDER_NONE);
        float d[4];
        draw(&s, 32, 35, 320, 200, d);
        long bad = 0;
        for (int y = 0; y < win_h; y++)
            for (int x = 0; x < win_w; x++)
                bad += !same(px(x, y), src[(35 + y / 3) * FE_W + 32 + x / 3], 0);
        CHECK(d[2] == 960 && d[3] == 600 && bad == 0, "no border crop: %ld pixels wrong", bad);
    }

    /* smooth: a bilinear picture; at texel centres (odd scale) the source */
    {
        pattern();
        window(FE_W * 3, FE_H * 3);
        fe_settings_t s = settings(FE_SCALE_INTEGER, FE_FILTER_SMOOTH, FE_BORDER_FULL);
        float d[4];
        draw(&s, 0, 0, FE_W, FE_H, d);
        long bad = 0, blended = 0;
        for (int y = 1; y < win_h; y += 3)
            for (int x = 1; x < win_w; x += 3)
                bad += !same(px(x, y), src[(y / 3) * FE_W + x / 3], 1);
        for (int x = 0; x < win_w; x++)
            blended += !same(px(x, 0), src[x / 3], 1);
        CHECK(bad == 0, "smooth: %ld texel centres wrong", bad);
        CHECK(blended > 0, "smooth: blends between pixels");
    }

    /* scanlines at 3x: per C64 line the middle row is bright, the edge rows
     * are darker by the same amount */
    {
        solid(rgba(200, 200, 200));
        window(FE_W * 3, FE_H * 3);
        fe_settings_t s = settings(FE_SCALE_INTEGER, FE_FILTER_SCANLINES, FE_BORDER_FULL);
        float d[4];
        draw(&s, 0, 0, FE_W, FE_H, d);
        /* k = (1 - 2 d^2) * 1.15, d = -1/3, 0, 1/3 */
        int top = px(100, 300)[0], mid = px(100, 301)[0], bot = px(100, 302)[0];
        int want_mid = (int)lroundf(200 * 1.15f), want_edge = (int)lroundf(200 * (1 - 2.0f / 9) * 1.15f);
        CHECK(abs(mid - want_mid) <= 2 && abs(top - want_edge) <= 2 && abs(bot - want_edge) <= 2,
              "scanline rows %d %d %d (want %d %d %d)", top, mid, bot, want_edge, want_mid, want_edge);
        long uneven = 0;
        for (int y = 0; y < win_h; y++)
            uneven += px(5, y)[0] != px(win_w - 5, y)[0];
        CHECK(uneven == 0, "scanlines are the same over the whole width");
        /* at 1x there are no scanlines */
        window(FE_W, FE_H);
        draw(&s, 0, 0, FE_W, FE_H, d);
        CHECK(same(px(10, 10), rgba(200, 200, 200), 1) && same(px(10, 11), rgba(200, 200, 200), 1),
              "no scanlines at 1x");
    }

    /* integer scale on a small screen falls back to fit */
    {
        pattern();
        window(300, 200);
        fe_settings_t s = settings(FE_SCALE_INTEGER, FE_FILTER_SHARP, FE_BORDER_FULL);
        float d[4];
        draw(&s, 0, 0, FE_W, FE_H, d);
        CHECK(d[2] <= 300 && d[3] <= 200 && (d[2] == 300 || d[3] == 200), "small screen fit %.0f x %.0f", d[2], d[3]);
    }

    glview_forget();
    CHECK(!glview_ready(), "forget");
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
