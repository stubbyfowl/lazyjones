/*
 * main.c - Android NativeActivity: lifecycle, game thread, OpenGL ES 2
 * renderer and input.
 *
 * Threads:
 *  - main (Java UI) thread: activity callbacks and input events. Input is
 *    stored in g_app under the mutex.
 *  - game thread: emulation at 50.125 Hz, rendering, audio feeding. It owns
 *    EGL. When the window goes away the main thread waits until the game
 *    thread has released its EGL surface.
 */
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <jni.h>

#include "audio.h"
#include "fe.h"
#include "glview.h"
#include "lj.h"

#define TAG "LazyJones"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define MAX_POINTERS 10

typedef struct {
    int32_t id;
    int active;
    int role;        /* 1 joystick, 2 fire, 3 menu corner */
    float x0, y0, x, y;
    double t0;
} touch_t;

typedef struct {
    ANativeActivity *activity;
    pthread_t thread;
    int thread_started;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    ANativeWindow *window;    /* set by the main thread */
    ANativeWindow *gl_window; /* window the EGL surface was made for */
    int resumed;
    int focused;
    int quit;
    AInputQueue *queue;
    /* input state (under mu) */
    uint32_t keys;
    uint32_t latched; /* keys pressed since the last snapshot (short taps) */
    int fire_touched; /* the fire half was touched since the last snapshot */
    float ax, ay, hx, hy;
    uint32_t trig;    /* L2/R2 from analog trigger axes */
    touch_t touch[MAX_POINTERS];
    int taps;
    float tap_x[8], tap_y[8];
    int corner_taps;
    /* image placement on screen, for touch (written by the game thread) */
    float dst_x, dst_y, dst_w, dst_h;
    int src_x, src_y, src_w, src_h;
    int win_w, win_h;
    char data_dir[512];
    int sdk;
} app_t;

static app_t g_app;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- EGL ------------------------------------------------------------------- */

typedef struct {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;
} egl_t;

static egl_t gx = {EGL_NO_DISPLAY, 0, EGL_NO_CONTEXT, EGL_NO_SURFACE};

static void egl_destroy_surface(void)
{
    if (gx.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(gx.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (gx.surface != EGL_NO_SURFACE)
            eglDestroySurface(gx.display, gx.surface);
    }
    gx.surface = EGL_NO_SURFACE;
}

static void egl_terminate(void)
{
    egl_destroy_surface();
    if (gx.display != EGL_NO_DISPLAY) {
        if (gx.context != EGL_NO_CONTEXT) {
            /* GL objects die with the context */
            glview_forget();
            eglDestroyContext(gx.display, gx.context);
        }
        eglTerminate(gx.display);
    }
    gx.display = EGL_NO_DISPLAY;
    gx.context = EGL_NO_CONTEXT;
}

typedef int32_t (*set_frame_rate_fn)(ANativeWindow *, float, int8_t);

static int egl_attach(ANativeWindow *win)
{
    if (gx.display == EGL_NO_DISPLAY) {
        gx.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (gx.display == EGL_NO_DISPLAY || !eglInitialize(gx.display, NULL, NULL)) {
            LOGE("eglInitialize failed");
            gx.display = EGL_NO_DISPLAY;
            return 0;
        }
        const EGLint attr888[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                  EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        const EGLint attr565[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                  EGL_RED_SIZE, 5, EGL_GREEN_SIZE, 6, EGL_BLUE_SIZE, 5, EGL_NONE};
        EGLint n = 0;
        if (!eglChooseConfig(gx.display, attr888, &gx.config, 1, &n) || n < 1) {
            if (!eglChooseConfig(gx.display, attr565, &gx.config, 1, &n) || n < 1) {
                LOGE("no EGL config");
                egl_terminate();
                return 0;
            }
        }
    }
    EGLint format = 0;
    eglGetConfigAttrib(gx.display, gx.config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(win, 0, 0, format);
    gx.surface = eglCreateWindowSurface(gx.display, gx.config, win, NULL);
    if (gx.surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%x", eglGetError());
        return 0;
    }
    if (gx.context == EGL_NO_CONTEXT) {
        const EGLint cattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        gx.context = eglCreateContext(gx.display, gx.config, EGL_NO_CONTEXT, cattr);
        if (gx.context == EGL_NO_CONTEXT) {
            LOGE("eglCreateContext failed");
            egl_destroy_surface();
            return 0;
        }
    }
    if (!eglMakeCurrent(gx.display, gx.surface, gx.surface, gx.context)) {
        EGLint err = eglGetError();
        LOGE("eglMakeCurrent failed: 0x%x", err);
        if (err == EGL_CONTEXT_LOST) {
            egl_terminate();
        } else {
            egl_destroy_surface();
        }
        return 0;
    }
    eglSwapInterval(gx.display, 1);
    if (!glview_ready() && !glview_init()) {
        LOGE("GL setup failed");
        egl_destroy_surface();
        return 0;
    }
    /* ask for a 50 Hz display mode when the device has one (Android 11+) */
    set_frame_rate_fn sfr = (set_frame_rate_fn)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
    if (sfr)
        sfr(win, (float)LJ_FRAME_HZ, 1 /* FIXED_SOURCE */);
    return 1;
}

static void render(void)
{
    EGLint ww = 0, wh = 0;
    eglQuerySurface(gx.display, gx.surface, EGL_WIDTH, &ww);
    eglQuerySurface(gx.display, gx.surface, EGL_HEIGHT, &wh);
    if (ww <= 0 || wh <= 0)
        return;
    const fe_settings_t *s = fe_get_settings();
    int sx, sy, sw, sh;
    fe_view(&sx, &sy, &sw, &sh);
    float dx, dy, dw, dh;
    glview_dest(ww, wh, sw, sh, s, &dx, &dy, &dw, &dh);

    pthread_mutex_lock(&g_app.mu);
    g_app.dst_x = dx; g_app.dst_y = dy; g_app.dst_w = dw; g_app.dst_h = dh;
    g_app.src_x = sx; g_app.src_y = sy; g_app.src_w = sw; g_app.src_h = sh;
    g_app.win_w = ww; g_app.win_h = wh;
    pthread_mutex_unlock(&g_app.mu);

    glview_draw(ww, wh, fe_rgba(), sx, sy, sw, sh, s, dx, dy, dw, dh);
}

/* ---- game thread ------------------------------------------------------------- */

static void push_audio(void)
{
    int16_t buf[2048];
    size_t n;
    while ((n = fe_audio_read(buf, sizeof buf / sizeof buf[0])) > 0)
        audio_write(buf, n);
}

static void *game_main(void *arg)
{
    app_t *a = (app_t *)arg;
    int rate = audio_open(a->activity->vm, a->sdk);
    int have_audio = rate > 0;
    if (!have_audio)
        rate = 48000;
    if (fe_init(a->data_dir, rate) != 0)
        LOGE("game data missing in this build");
    fe_set_output_rate(rate);
    if (have_audio)
        audio_pause();

    const double period = 1.0 / LJ_FRAME_HZ;
    double next = now_sec();
    int running = 0;
    /* key presses not yet seen by a frame: a tap shorter than one frame
     * (down and up between two snapshots) still counts as one press */
    uint32_t pending = 0;
    int pending_touch_fire = 0;
    for (;;) {
        pthread_mutex_lock(&a->mu);
        for (;;) {
            /* give the surface back when the window changed or went away */
            if (gx.surface != EGL_NO_SURFACE && a->gl_window != a->window) {
                egl_destroy_surface();
                a->gl_window = NULL;
                pthread_cond_broadcast(&a->cv);
            }
            if (a->quit || (a->window && a->resumed))
                break;
            if (running) {
                running = 0;
                pthread_mutex_unlock(&a->mu);
                if (have_audio)
                    audio_pause();
                fe_pause();
                pthread_mutex_lock(&a->mu);
                continue;
            }
            pthread_cond_wait(&a->cv, &a->mu);
        }
        if (a->quit) {
            pthread_mutex_unlock(&a->mu);
            break;
        }
        ANativeWindow *win = a->window;
        /* input snapshot */
        fe_input_t in;
        memset(&in, 0, sizeof in);
        pending |= a->latched;
        a->latched = 0;
        pending_touch_fire |= a->fire_touched;
        a->fire_touched = 0;
        in.held = a->keys | pending | a->trig;
        in.ax = a->ax;
        in.ay = a->ay;
        if (a->hx < -0.5f) in.held |= FE_BTN_LEFT;
        if (a->hx > 0.5f) in.held |= FE_BTN_RIGHT;
        if (a->hy < -0.5f) in.held |= FE_BTN_UP;
        if (a->hy > 0.5f) in.held |= FE_BTN_DOWN;
        int menu = fe_menu_open();
        if (menu)
            pending_touch_fire = 0; /* touches are menu taps there */
        else if (pending_touch_fire)
            in.held |= FE_BTN_TOUCH_FIRE;
        if (!menu) {
            for (int i = 0; i < MAX_POINTERS; i++) {
                touch_t *t = &a->touch[i];
                if (!t->active)
                    continue;
                if (t->role == 2)
                    in.held |= FE_BTN_TOUCH_FIRE;
                if (t->role == 1) {
                    float k = (float)(a->win_h > 0 ? a->win_h : 1000) * 0.06f;
                    float jx = (t->x - t->x0) / k, jy = (t->y - t->y0) / k;
                    if (jx > 1) jx = 1;
                    if (jx < -1) jx = -1;
                    if (jy > 1) jy = 1;
                    if (jy < -1) jy = -1;
                    if (fabsf(jx) > fabsf(in.ax)) in.ax = jx;
                    if (fabsf(jy) > fabsf(in.ay)) in.ay = jy;
                }
            }
        }
        int ntaps = a->taps;
        float tx[8], ty[8];
        memcpy(tx, a->tap_x, sizeof tx);
        memcpy(ty, a->tap_y, sizeof ty);
        a->taps = 0;
        int corner = a->corner_taps;
        a->corner_taps = 0;
        float dx = a->dst_x, dy = a->dst_y, dw = a->dst_w, dh = a->dst_h;
        int sx = a->src_x, sy = a->src_y, sw = a->src_w, sh = a->src_h;
        pthread_mutex_unlock(&a->mu);

        if (!running) {
            running = 1;
            if (have_audio)
                audio_resume();
            next = now_sec();
        }
        if (gx.surface == EGL_NO_SURFACE) {
            if (!egl_attach(win)) {
                usleep(100000);
                continue;
            }
            pthread_mutex_lock(&a->mu);
            a->gl_window = win;
            pthread_mutex_unlock(&a->mu);
        }

        /* touch: corner opens the menu, taps choose menu lines */
        if (corner)
            fe_touch_menu();
        for (int i = 0; i < ntaps && i < 8; i++) {
            if (!fe_menu_open() || dw <= 0 || dh <= 0)
                break;
            int ix = sx + (int)((tx[i] - dx) / dw * (float)sw);
            int iy = sy + (int)((ty[i] - dy) / dh * (float)sh);
            fe_menu_tap(ix, iy);
        }

        fe_set_input(&in);
        double t = now_sec();
        int frames = 0;
        while (t >= next && frames < 3) {
            fe_frame();
            push_audio();
            next += period;
            frames++;
        }
        if (frames > 0) {
            pending = 0;
            pending_touch_fire = 0;
        }
        if (t - next > 0.25)
            next = t; /* far behind (debugger, slow device): do not race */
        if (fe_quit_requested()) {
            fe_pause();
            running = 0;
            if (have_audio) {
                audio_close();
                have_audio = 0;
            }
            /* release the window before asking Android to finish, so that
             * onNativeWindowDestroyed never waits for us */
            egl_terminate();
            pthread_mutex_lock(&a->mu);
            a->gl_window = NULL;
            pthread_cond_broadcast(&a->cv);
            pthread_mutex_unlock(&a->mu);
            ANativeActivity_finish(a->activity);
            pthread_mutex_lock(&a->mu);
            while (!a->quit)
                pthread_cond_wait(&a->cv, &a->mu);
            pthread_mutex_unlock(&a->mu);
            break;
        }
        if (have_audio) {
            if (audio_lost()) {
                audio_close();
                int r = audio_open(a->activity->vm, a->sdk);
                have_audio = r > 0;
                if (have_audio) {
                    rate = r;
                    fe_set_output_rate(rate);
                }
            } else {
                /* keep about 50 ms queued: nudge the emulated sample rate
                 * by up to 0.5 % (inaudible) instead of dropping samples.
                 * Too much queued -> produce fewer samples per second. */
                double target = rate * 0.05;
                double err = ((double)audio_queued() - target) / target;
                if (err > 1) err = 1;
                if (err < -1) err = -1;
                fe_set_sample_rate(rate * (1.0 - 0.005 * err));
            }
        }
        render();
        if (!eglSwapBuffers(gx.display, gx.surface)) {
            EGLint err = eglGetError();
            LOGE("eglSwapBuffers: 0x%x", err);
            if (err == EGL_CONTEXT_LOST)
                egl_terminate();
            else
                egl_destroy_surface();
            pthread_mutex_lock(&a->mu);
            a->gl_window = NULL;
            pthread_cond_broadcast(&a->cv);
            pthread_mutex_unlock(&a->mu);
        }
        if (frames == 0) {
            double wait = next - now_sec();
            if (wait > 0.0005)
                usleep((useconds_t)(wait * 1e6 * 0.8));
        }
    }
    if (running)
        fe_pause();
    if (have_audio)
        audio_close();
    egl_terminate();
    pthread_mutex_lock(&a->mu);
    a->gl_window = NULL;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
    return NULL;
}

/* ---- input ----------------------------------------------------------------- */

/* Android key -> physical button of the front end. Keyboards act like a
 * gamepad: their keys follow the button mapping of the CONTROLS menu. */
static uint32_t key_button(int32_t code)
{
    switch (code) {
    case AKEYCODE_DPAD_UP: case AKEYCODE_W: return FE_BTN_UP;
    case AKEYCODE_DPAD_DOWN: case AKEYCODE_S: return FE_BTN_DOWN;
    case AKEYCODE_DPAD_LEFT: case AKEYCODE_A: return FE_BTN_LEFT;
    case AKEYCODE_DPAD_RIGHT: case AKEYCODE_D: return FE_BTN_RIGHT;
    case AKEYCODE_BUTTON_A: case AKEYCODE_DPAD_CENTER: case AKEYCODE_ENTER:
    case AKEYCODE_NUMPAD_ENTER: case AKEYCODE_SPACE: case AKEYCODE_CTRL_LEFT:
    case AKEYCODE_CTRL_RIGHT: case AKEYCODE_BUTTON_1:
        return FE_BTN_A;
    case AKEYCODE_BUTTON_B: case AKEYCODE_BUTTON_2: return FE_BTN_B;
    case AKEYCODE_BUTTON_X: case AKEYCODE_BUTTON_C: case AKEYCODE_BUTTON_3: return FE_BTN_X;
    case AKEYCODE_BUTTON_Y: case AKEYCODE_BUTTON_Z: case AKEYCODE_BUTTON_4: return FE_BTN_Y;
    case AKEYCODE_BUTTON_L1: case AKEYCODE_TAB: return FE_BTN_L1;
    case AKEYCODE_BUTTON_R1: case AKEYCODE_M: return FE_BTN_R1;
    case AKEYCODE_BUTTON_L2: return FE_BTN_L2;
    case AKEYCODE_BUTTON_R2: return FE_BTN_R2;
    case AKEYCODE_BUTTON_START: case AKEYCODE_P: return FE_BTN_START;
    case AKEYCODE_BUTTON_SELECT: return FE_BTN_SELECT;
    case AKEYCODE_BACK: case AKEYCODE_MENU: case AKEYCODE_ESCAPE: case AKEYCODE_BUTTON_MODE:
        return FE_BTN_MENU;
    default: return 0;
    }
}

static float axis_or_zero(const AInputEvent *e, int32_t axis)
{
    float v = AMotionEvent_getAxisValue(e, axis, 0);
    return isnan(v) ? 0.0f : v;
}

static void touch_down(app_t *a, int32_t id, float x, float y)
{
    int slot = -1;
    for (int i = 0; i < MAX_POINTERS; i++)
        if (!a->touch[i].active) {
            slot = i;
            break;
        }
    if (slot < 0)
        return;
    touch_t *t = &a->touch[slot];
    t->id = id;
    t->active = 1;
    t->x0 = t->x = x;
    t->y0 = t->y = y;
    t->t0 = now_sec();
    float w = (float)(a->win_w > 0 ? a->win_w : 1), h = (float)(a->win_h > 0 ? a->win_h : 1);
    if (x > w * 0.85f && y < h * 0.15f) {
        t->role = 3;
    } else if (x < w * 0.5f) {
        t->role = 1;
    } else {
        t->role = 2;
        a->fire_touched = 1;
    }
}

static void touch_up(app_t *a, int32_t id, int cancelled)
{
    for (int i = 0; i < MAX_POINTERS; i++) {
        touch_t *t = &a->touch[i];
        if (!t->active || t->id != id)
            continue;
        float d = fabsf(t->x - t->x0) + fabsf(t->y - t->y0);
        float h = (float)(a->win_h > 0 ? a->win_h : 1000);
        int tap = !cancelled && d < h * 0.03f && now_sec() - t->t0 < 0.5;
        if (tap && a->taps < 8) {
            a->tap_x[a->taps] = t->x;
            a->tap_y[a->taps] = t->y;
            a->taps++;
        }
        if (tap && t->role == 3)
            a->corner_taps++;
        t->active = 0;
    }
}

static int handle_input(app_t *a, AInputEvent *e)
{
    int32_t type = AInputEvent_getType(e);
    int32_t src = AInputEvent_getSource(e);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(e);
        int32_t action = AKeyEvent_getAction(e);
        uint32_t b = key_button(code);
        if (!b)
            return 0; /* volume keys etc. go to the system */
        pthread_mutex_lock(&a->mu);
        if (action == AKEY_EVENT_ACTION_DOWN) {
            a->keys |= b;
            a->latched |= b;
        }
        else if (action == AKEY_EVENT_ACTION_UP)
            a->keys &= ~b;
        pthread_mutex_unlock(&a->mu);
        pthread_cond_broadcast(&a->cv);
        return 1;
    }
    if (type != AINPUT_EVENT_TYPE_MOTION)
        return 0;
    if ((src & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK ||
        (src & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD) {
        pthread_mutex_lock(&a->mu);
        a->ax = axis_or_zero(e, AMOTION_EVENT_AXIS_X);
        a->ay = axis_or_zero(e, AMOTION_EVENT_AXIS_Y);
        a->hx = axis_or_zero(e, AMOTION_EVENT_AXIS_HAT_X);
        a->hy = axis_or_zero(e, AMOTION_EVENT_AXIS_HAT_Y);
        /* analog triggers (some handhelds have no L2/R2 key events) */
        float lt = fmaxf(axis_or_zero(e, AMOTION_EVENT_AXIS_LTRIGGER),
                         axis_or_zero(e, AMOTION_EVENT_AXIS_BRAKE));
        float rt = fmaxf(axis_or_zero(e, AMOTION_EVENT_AXIS_RTRIGGER),
                         axis_or_zero(e, AMOTION_EVENT_AXIS_GAS));
        uint32_t old = a->trig;
        if (lt > 0.5f) a->trig |= FE_BTN_L2;
        else if (lt < 0.3f) a->trig &= ~(uint32_t)FE_BTN_L2;
        if (rt > 0.5f) a->trig |= FE_BTN_R2;
        else if (rt < 0.3f) a->trig &= ~(uint32_t)FE_BTN_R2;
        a->latched |= a->trig & ~old; /* a short pull still counts */
        pthread_mutex_unlock(&a->mu);
        return 1;
    }
    if ((src & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) {
        int32_t action = AMotionEvent_getAction(e);
        int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
        size_t idx = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                              AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        pthread_mutex_lock(&a->mu);
        switch (masked) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            touch_down(a, AMotionEvent_getPointerId(e, idx), AMotionEvent_getX(e, idx),
                       AMotionEvent_getY(e, idx));
            break;
        case AMOTION_EVENT_ACTION_MOVE: {
            size_t n = AMotionEvent_getPointerCount(e);
            for (size_t p = 0; p < n; p++) {
                int32_t id = AMotionEvent_getPointerId(e, p);
                for (int i = 0; i < MAX_POINTERS; i++)
                    if (a->touch[i].active && a->touch[i].id == id) {
                        a->touch[i].x = AMotionEvent_getX(e, p);
                        a->touch[i].y = AMotionEvent_getY(e, p);
                    }
            }
            break;
        }
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            touch_up(a, AMotionEvent_getPointerId(e, idx), 0);
            break;
        case AMOTION_EVENT_ACTION_CANCEL:
            for (int i = 0; i < MAX_POINTERS; i++)
                a->touch[i].active = 0;
            break;
        default:
            break;
        }
        pthread_mutex_unlock(&a->mu);
        return 1;
    }
    return 0;
}

static int input_cb(int fd, int events, void *data)
{
    (void)fd;
    (void)events;
    app_t *a = (app_t *)data;
    AInputEvent *e = NULL;
    while (a->queue && AInputQueue_getEvent(a->queue, &e) >= 0) {
        if (AInputQueue_preDispatchEvent(a->queue, e))
            continue;
        int handled = handle_input(a, e);
        AInputQueue_finishEvent(a->queue, e, handled);
    }
    return 1;
}

/* ---- immersive full screen (JNI, main thread) ------------------------------ */

static void clear_exc(JNIEnv *env)
{
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
}

static void set_immersive(ANativeActivity *act)
{
    JNIEnv *env = act->env;
    if (!env)
        return;
    jclass act_cls = (*env)->GetObjectClass(env, act->clazz);
    jmethodID get_window = (*env)->GetMethodID(env, act_cls, "getWindow", "()Landroid/view/Window;");
    clear_exc(env);
    if (!get_window) {
        (*env)->DeleteLocalRef(env, act_cls);
        return;
    }
    jobject window = (*env)->CallObjectMethod(env, act->clazz, get_window);
    clear_exc(env);
    if (window) {
        jclass win_cls = (*env)->GetObjectClass(env, window);
        int done = 0;
        if (act->sdkVersion >= 30) {
            jmethodID get_ic = (*env)->GetMethodID(env, win_cls, "getInsetsController",
                                                   "()Landroid/view/WindowInsetsController;");
            clear_exc(env);
            jobject ic = get_ic ? (*env)->CallObjectMethod(env, window, get_ic) : NULL;
            clear_exc(env);
            jclass type_cls = (*env)->FindClass(env, "android/view/WindowInsets$Type");
            clear_exc(env);
            if (ic && type_cls) {
                jmethodID sys_bars = (*env)->GetStaticMethodID(env, type_cls, "systemBars", "()I");
                clear_exc(env);
                jclass ic_cls = (*env)->GetObjectClass(env, ic);
                jmethodID hide = (*env)->GetMethodID(env, ic_cls, "hide", "(I)V");
                jmethodID behavior = (*env)->GetMethodID(env, ic_cls, "setSystemBarsBehavior", "(I)V");
                clear_exc(env);
                if (sys_bars && hide && behavior) {
                    jint types = (*env)->CallStaticIntMethod(env, type_cls, sys_bars);
                    clear_exc(env);
                    (*env)->CallVoidMethod(env, ic, behavior, 2); /* show transient bars by swipe */
                    clear_exc(env);
                    (*env)->CallVoidMethod(env, ic, hide, types);
                    clear_exc(env);
                    done = 1;
                }
                (*env)->DeleteLocalRef(env, ic_cls);
            }
            if (type_cls)
                (*env)->DeleteLocalRef(env, type_cls);
            if (ic)
                (*env)->DeleteLocalRef(env, ic);
        }
        if (!done) {
            jmethodID get_decor = (*env)->GetMethodID(env, win_cls, "getDecorView", "()Landroid/view/View;");
            clear_exc(env);
            jobject decor = get_decor ? (*env)->CallObjectMethod(env, window, get_decor) : NULL;
            clear_exc(env);
            if (decor) {
                jclass view_cls = (*env)->GetObjectClass(env, decor);
                jmethodID suv = (*env)->GetMethodID(env, view_cls, "setSystemUiVisibility", "(I)V");
                clear_exc(env);
                /* LAYOUT_STABLE | LAYOUT_HIDE_NAVIGATION | LAYOUT_FULLSCREEN |
                 * HIDE_NAVIGATION | FULLSCREEN | IMMERSIVE_STICKY */
                if (suv)
                    (*env)->CallVoidMethod(env, decor, suv, 0x1706);
                clear_exc(env);
                (*env)->DeleteLocalRef(env, view_cls);
                (*env)->DeleteLocalRef(env, decor);
            }
        }
        (*env)->DeleteLocalRef(env, win_cls);
        (*env)->DeleteLocalRef(env, window);
    }
    (*env)->DeleteLocalRef(env, act_cls);
}

/* ---- activity callbacks (main thread) -------------------------------------- */

static void on_start(ANativeActivity *act) { (void)act; }

static void on_resume(ANativeActivity *act)
{
    app_t *a = (app_t *)act->instance;
    pthread_mutex_lock(&a->mu);
    a->resumed = 1;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
    set_immersive(act);
}

/* forget held keys, sticks and touches (caller holds a->mu): the release
 * events can get lost when the app goes to the background */
static void clear_input(app_t *a)
{
    a->keys = 0;
    a->latched = 0;
    a->fire_touched = 0;
    a->trig = 0;
    a->ax = a->ay = a->hx = a->hy = 0.0f;
    for (int i = 0; i < MAX_POINTERS; i++)
        a->touch[i].active = 0;
}

static void on_pause(ANativeActivity *act)
{
    app_t *a = (app_t *)act->instance;
    pthread_mutex_lock(&a->mu);
    a->resumed = 0;
    clear_input(a);
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
}

static void on_stop(ANativeActivity *act) { (void)act; }

static void on_destroy(ANativeActivity *act)
{
    app_t *a = (app_t *)act->instance;
    pthread_mutex_lock(&a->mu);
    a->quit = 1;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
    if (a->thread_started) {
        pthread_join(a->thread, NULL);
        a->thread_started = 0;
    }
}

static void on_focus(ANativeActivity *act, int has_focus)
{
    app_t *a = (app_t *)act->instance;
    pthread_mutex_lock(&a->mu);
    a->focused = has_focus;
    if (!has_focus)
        clear_input(a);
    pthread_mutex_unlock(&a->mu);
    if (has_focus)
        set_immersive(act);
}

static void on_window_created(ANativeActivity *act, ANativeWindow *win)
{
    app_t *a = (app_t *)act->instance;
    pthread_mutex_lock(&a->mu);
    a->window = win;
    pthread_cond_broadcast(&a->cv);
    pthread_mutex_unlock(&a->mu);
}

static void on_window_destroyed(ANativeActivity *act, ANativeWindow *win)
{
    (void)win;
    app_t *a = (app_t *)act->instance;
    pthread_mutex_lock(&a->mu);
    a->window = NULL;
    pthread_cond_broadcast(&a->cv);
    /* the game thread must stop using the surface before we return */
    while (a->gl_window != NULL && a->thread_started && !a->quit)
        pthread_cond_wait(&a->cv, &a->mu);
    pthread_mutex_unlock(&a->mu);
}

static void on_input_created(ANativeActivity *act, AInputQueue *q)
{
    app_t *a = (app_t *)act->instance;
    a->queue = q;
    AInputQueue_attachLooper(q, ALooper_forThread(), 1, input_cb, a);
}

static void on_input_destroyed(ANativeActivity *act, AInputQueue *q)
{
    app_t *a = (app_t *)act->instance;
    AInputQueue_detachLooper(q);
    a->queue = NULL;
}

static void *on_save_state(ANativeActivity *act, size_t *size)
{
    (void)act;
    *size = 0;
    return NULL;
}

static void lj_log_android(const char *msg) { LOGI("%s", msg); }
static void gl_log_android(const char *msg) { LOGE("%s", msg); }

JNIEXPORT void ANativeActivity_onCreate(ANativeActivity *act, void *saved, size_t saved_size)
{
    (void)saved;
    (void)saved_size;
    app_t *a = &g_app;
    if (a->thread_started) {
        /* the previous instance was not destroyed cleanly: stop it */
        pthread_mutex_lock(&a->mu);
        a->quit = 1;
        pthread_cond_broadcast(&a->cv);
        pthread_mutex_unlock(&a->mu);
        pthread_join(a->thread, NULL);
    }
    memset(a, 0, sizeof *a);
    a->activity = act;
    a->sdk = act->sdkVersion;
    pthread_mutex_init(&a->mu, NULL);
    pthread_cond_init(&a->cv, NULL);
    snprintf(a->data_dir, sizeof a->data_dir, "%s", act->internalDataPath ? act->internalDataPath : ".");
    act->instance = a;
    act->callbacks->onStart = on_start;
    act->callbacks->onResume = on_resume;
    act->callbacks->onSaveInstanceState = on_save_state;
    act->callbacks->onPause = on_pause;
    act->callbacks->onStop = on_stop;
    act->callbacks->onDestroy = on_destroy;
    act->callbacks->onWindowFocusChanged = on_focus;
    act->callbacks->onNativeWindowCreated = on_window_created;
    act->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    act->callbacks->onInputQueueCreated = on_input_created;
    act->callbacks->onInputQueueDestroyed = on_input_destroyed;
    ANativeActivity_setWindowFlags(act, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    lj_set_log(lj_log_android);
    glview_set_log(gl_log_android);
    if (pthread_create(&a->thread, NULL, game_main, a) == 0)
        a->thread_started = 1;
    else
        LOGE("cannot start the game thread");
}
