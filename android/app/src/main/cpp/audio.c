/*
 * audio.c - sound output for Android (see audio.h).
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>
#include "audio.h"

#define TAG "LazyJones"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

/* ---- ring buffer (one producer, one consumer) ------------------------- */

#define RING_SIZE 16384u /* power of two */
#define RING_MASK (RING_SIZE - 1u)
static int16_t ring[RING_SIZE];
static atomic_uint ring_w, ring_r; /* free running indices */

void audio_write(const int16_t *s, size_t n)
{
    unsigned w = atomic_load_explicit(&ring_w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&ring_r, memory_order_acquire);
    unsigned space = RING_SIZE - (w - r);
    if (n > space)
        n = space;
    for (size_t i = 0; i < n; i++)
        ring[(w + (unsigned)i) & RING_MASK] = s[i];
    atomic_store_explicit(&ring_w, w + (unsigned)n, memory_order_release);
}

static size_t ring_read(int16_t *out, size_t n)
{
    unsigned r = atomic_load_explicit(&ring_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&ring_w, memory_order_acquire);
    unsigned avail = w - r;
    if (n > avail)
        n = avail;
    for (size_t i = 0; i < n; i++)
        out[i] = ring[(r + (unsigned)i) & RING_MASK];
    atomic_store_explicit(&ring_r, r + (unsigned)n, memory_order_release);
    return n;
}

size_t audio_queued(void)
{
    unsigned w = atomic_load_explicit(&ring_w, memory_order_acquire);
    unsigned r = atomic_load_explicit(&ring_r, memory_order_acquire);
    return (size_t)(w - r);
}

static void ring_clear(void)
{
    atomic_store(&ring_r, atomic_load(&ring_w));
}

static atomic_int lost;
static atomic_int paused;

int audio_lost(void) { return atomic_load(&lost); }

/* ---- AAudio (loaded at run time) ---------------------------------------- */

typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef struct AAudioStreamStruct AAudioStream;
typedef int32_t aa_result;
typedef int32_t (*aa_data_cb)(AAudioStream *, void *, void *, int32_t);
typedef void (*aa_error_cb)(AAudioStream *, void *, aa_result);

#define AA_FORMAT_PCM_I16 1
#define AA_FORMAT_PCM_FLOAT 2
#define AA_SHARING_SHARED 1
#define AA_PERF_LOW_LATENCY 12
#define AA_CALLBACK_CONTINUE 0

static struct {
    void *lib;
    aa_result (*create_builder)(AAudioStreamBuilder **);
    void (*set_perf)(AAudioStreamBuilder *, int32_t);
    void (*set_sharing)(AAudioStreamBuilder *, int32_t);
    void (*set_format)(AAudioStreamBuilder *, int32_t);
    void (*set_channels)(AAudioStreamBuilder *, int32_t);
    void (*set_data_cb)(AAudioStreamBuilder *, aa_data_cb, void *);
    void (*set_error_cb)(AAudioStreamBuilder *, aa_error_cb, void *);
    aa_result (*open)(AAudioStreamBuilder *, AAudioStream **);
    aa_result (*builder_delete)(AAudioStreamBuilder *);
    aa_result (*start)(AAudioStream *);
    aa_result (*pause)(AAudioStream *);
    aa_result (*stop)(AAudioStream *);
    aa_result (*close)(AAudioStream *);
    int32_t (*get_rate)(AAudioStream *);
    int32_t (*get_burst)(AAudioStream *);
    aa_result (*set_buffer_size)(AAudioStream *, int32_t);
    int32_t (*get_channels)(AAudioStream *);
    int32_t (*get_format)(AAudioStream *);
} aa;

static AAudioStream *aa_stream;
static int aa_channels, aa_format;

#define TMP_FRAMES 1024
static int16_t tmp[TMP_FRAMES];

static int32_t aa_callback(AAudioStream *s, void *user, void *data, int32_t frames)
{
    (void)s;
    (void)user;
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > TMP_FRAMES)
            n = TMP_FRAMES;
        size_t got = atomic_load(&paused) ? 0 : ring_read(tmp, (size_t)n);
        for (int i = (int)got; i < n; i++)
            tmp[i] = 0;
        if (aa_format == AA_FORMAT_PCM_FLOAT) {
            float *out = (float *)data + (size_t)done * (size_t)aa_channels;
            for (int i = 0; i < n; i++) {
                float v = (float)tmp[i] * (1.0f / 32768.0f);
                for (int c = 0; c < aa_channels; c++)
                    *out++ = v;
            }
        } else {
            int16_t *out = (int16_t *)data + (size_t)done * (size_t)aa_channels;
            for (int i = 0; i < n; i++)
                for (int c = 0; c < aa_channels; c++)
                    *out++ = tmp[i];
        }
        done += n;
    }
    return AA_CALLBACK_CONTINUE;
}

static void aa_error(AAudioStream *s, void *user, aa_result err)
{
    (void)s;
    (void)user;
    LOGW("AAudio error %d", (int)err);
    atomic_store(&lost, 1);
}

static int aa_load(void)
{
    if (aa.lib)
        return 1;
    void *lib = dlopen("libaaudio.so", RTLD_NOW);
    if (!lib)
        return 0;
#define SYM(field, name) do { *(void **)&aa.field = dlsym(lib, name); if (!aa.field) { dlclose(lib); return 0; } } while (0)
    SYM(create_builder, "AAudio_createStreamBuilder");
    SYM(set_perf, "AAudioStreamBuilder_setPerformanceMode");
    SYM(set_sharing, "AAudioStreamBuilder_setSharingMode");
    SYM(set_format, "AAudioStreamBuilder_setFormat");
    SYM(set_channels, "AAudioStreamBuilder_setChannelCount");
    SYM(set_data_cb, "AAudioStreamBuilder_setDataCallback");
    SYM(set_error_cb, "AAudioStreamBuilder_setErrorCallback");
    SYM(open, "AAudioStreamBuilder_openStream");
    SYM(builder_delete, "AAudioStreamBuilder_delete");
    SYM(start, "AAudioStream_requestStart");
    SYM(pause, "AAudioStream_requestPause");
    SYM(stop, "AAudioStream_requestStop");
    SYM(close, "AAudioStream_close");
    SYM(get_rate, "AAudioStream_getSampleRate");
    SYM(get_burst, "AAudioStream_getFramesPerBurst");
    SYM(set_buffer_size, "AAudioStream_setBufferSizeInFrames");
    SYM(get_channels, "AAudioStream_getChannelCount");
    SYM(get_format, "AAudioStream_getFormat");
#undef SYM
    aa.lib = lib;
    return 1;
}

static int aa_open(void)
{
    if (!aa_load())
        return 0;
    AAudioStreamBuilder *b = NULL;
    if (aa.create_builder(&b) != 0 || !b)
        return 0;
    aa.set_perf(b, AA_PERF_LOW_LATENCY);
    aa.set_sharing(b, AA_SHARING_SHARED);
    aa.set_format(b, AA_FORMAT_PCM_I16);
    aa.set_channels(b, 1);
    aa.set_data_cb(b, aa_callback, NULL);
    aa.set_error_cb(b, aa_error, NULL);
    AAudioStream *s = NULL;
    aa_result r = aa.open(b, &s);
    aa.builder_delete(b);
    if (r != 0 || !s) {
        LOGW("AAudio open failed: %d", (int)r);
        return 0;
    }
    aa_channels = aa.get_channels(s);
    aa_format = aa.get_format(s);
    int rate = aa.get_rate(s);
    if (aa_channels < 1 || aa_channels > 8 || rate < 8000 ||
        (aa_format != AA_FORMAT_PCM_I16 && aa_format != AA_FORMAT_PCM_FLOAT)) {
        aa.close(s);
        return 0;
    }
    int burst = aa.get_burst(s);
    if (burst > 0)
        aa.set_buffer_size(s, burst * 2);
    aa_stream = s;
    if (aa.start(s) != 0) {
        aa.close(s);
        aa_stream = NULL;
        return 0;
    }
    LOGI("AAudio: %d Hz, %d channels, format %d, burst %d", rate, aa_channels, aa_format, burst);
    return rate;
}

/* ---- AudioTrack through JNI (Android 8 and older) ------------------------ */

static JavaVM *jvm;
static pthread_t at_thread;
static atomic_int at_run;
static int at_rate;

static void *at_main(void *arg)
{
    (void)arg;
    JNIEnv *env = NULL;
    if ((*jvm)->AttachCurrentThread(jvm, &env, NULL) != 0 || !env)
        return NULL;
    jclass cls = (*env)->FindClass(env, "android/media/AudioTrack");
    jobject track = NULL;
    jshortArray arr = NULL;
    jmethodID m_play = NULL, m_pause = NULL, m_write = NULL, m_stop = NULL, m_release = NULL;
    if (!cls)
        goto out;
    jmethodID m_min = (*env)->GetStaticMethodID(env, cls, "getMinBufferSize", "(III)I");
    jmethodID m_init = (*env)->GetMethodID(env, cls, "<init>", "(IIIIII)V");
    jmethodID m_state = (*env)->GetMethodID(env, cls, "getState", "()I");
    m_play = (*env)->GetMethodID(env, cls, "play", "()V");
    m_pause = (*env)->GetMethodID(env, cls, "pause", "()V");
    m_write = (*env)->GetMethodID(env, cls, "write", "([SII)I");
    m_stop = (*env)->GetMethodID(env, cls, "stop", "()V");
    m_release = (*env)->GetMethodID(env, cls, "release", "()V");
    if ((*env)->ExceptionCheck(env) || !m_min || !m_init || !m_state || !m_play || !m_pause ||
        !m_write || !m_stop || !m_release)
        goto out;
    /* STREAM_MUSIC = 3, CHANNEL_OUT_MONO = 4, ENCODING_PCM_16BIT = 2, MODE_STREAM = 1 */
    jint minb = (*env)->CallStaticIntMethod(env, cls, m_min, at_rate, 4, 2);
    if ((*env)->ExceptionCheck(env) || minb <= 0)
        goto out;
    jint bytes = minb * 2;
    if (bytes < at_rate / 10 * 2)
        bytes = at_rate / 10 * 2;
    track = (*env)->NewObject(env, cls, m_init, 3, at_rate, 4, 2, bytes, 1);
    if ((*env)->ExceptionCheck(env) || !track)
        goto out;
    if ((*env)->CallIntMethod(env, track, m_state) != 1)
        goto out;
    arr = (*env)->NewShortArray(env, 256);
    if (!arr)
        goto out;
    (*env)->CallVoidMethod(env, track, m_play);
    int playing = 1;
    int16_t buf[256];
    while (atomic_load(&at_run)) {
        if (atomic_load(&paused)) {
            if (playing) {
                (*env)->CallVoidMethod(env, track, m_pause);
                playing = 0;
            }
            usleep(20000);
            continue;
        }
        if (!playing) {
            (*env)->CallVoidMethod(env, track, m_play);
            playing = 1;
        }
        size_t got = 0;
        for (int tries = 0; tries < 10 && got < 256; tries++) {
            got += ring_read(buf + got, 256 - got);
            if (got < 256)
                usleep(2000);
        }
        for (size_t i = got; i < 256; i++)
            buf[i] = 0;
        (*env)->SetShortArrayRegion(env, arr, 0, 256, buf);
        (*env)->CallIntMethod(env, track, m_write, arr, 0, 256);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
            break;
        }
    }
    (*env)->CallVoidMethod(env, track, m_stop);
    (*env)->CallVoidMethod(env, track, m_release);
out:
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (arr)
        (*env)->DeleteLocalRef(env, arr);
    if (track)
        (*env)->DeleteLocalRef(env, track);
    if (cls)
        (*env)->DeleteLocalRef(env, cls);
    (*jvm)->DetachCurrentThread(jvm);
    return NULL;
}

static int at_native_rate(void)
{
    JNIEnv *env = NULL;
    int attached = 0;
    int rate = 44100;
    if ((*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*jvm)->AttachCurrentThread(jvm, &env, NULL) != 0)
            return rate;
        attached = 1;
    }
    jclass cls = (*env)->FindClass(env, "android/media/AudioTrack");
    if (cls) {
        jmethodID m = (*env)->GetStaticMethodID(env, cls, "getNativeOutputSampleRate", "(I)I");
        if (m) {
            jint r = (*env)->CallStaticIntMethod(env, cls, m, 3);
            if (!(*env)->ExceptionCheck(env) && r >= 8000 && r <= 192000)
                rate = r;
        }
        (*env)->DeleteLocalRef(env, cls);
    }
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (attached)
        (*jvm)->DetachCurrentThread(jvm);
    return rate;
}

static int at_open(JavaVM *vm)
{
    jvm = vm;
    at_rate = at_native_rate();
    atomic_store(&at_run, 1);
    if (pthread_create(&at_thread, NULL, at_main, NULL) != 0) {
        atomic_store(&at_run, 0);
        return 0;
    }
    LOGI("AudioTrack: %d Hz", at_rate);
    return at_rate;
}

/* ---- public ---------------------------------------------------------------- */

static int mode; /* 0 none, 1 AAudio, 2 AudioTrack */

int audio_open(JavaVM *vm, int sdk)
{
    atomic_store(&lost, 0);
    atomic_store(&paused, 0);
    ring_clear();
    int rate = 0;
    if (sdk >= 28) {
        rate = aa_open();
        if (rate) {
            mode = 1;
            return rate;
        }
    }
    if (vm) {
        rate = at_open(vm);
        if (rate) {
            mode = 2;
            return rate;
        }
    }
    mode = 0;
    LOGW("no audio output");
    return 0;
}

void audio_close(void)
{
    if (mode == 1 && aa_stream) {
        aa.stop(aa_stream);
        aa.close(aa_stream);
        aa_stream = NULL;
    } else if (mode == 2) {
        atomic_store(&at_run, 0);
        pthread_join(at_thread, NULL);
    }
    mode = 0;
    ring_clear();
}

void audio_pause(void)
{
    atomic_store(&paused, 1);
    if (mode == 1 && aa_stream)
        aa.pause(aa_stream);
}

void audio_resume(void)
{
    ring_clear();
    atomic_store(&paused, 0);
    if (mode == 1 && aa_stream)
        aa.start(aa_stream);
}
