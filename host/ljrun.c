/*
 * ljrun.c - headless host runner for testing the runtime.
 *
 *   ljrun [options]
 *     --prg FILE        load a PRG and start it (needs --entry)
 *     --entry HEX       entry address (BASIC SYS target)
 *     --game            start the embedded game (lj_game_init)
 *     --frames N        number of frames to run
 *     --shot LIST       comma separated frame numbers to save as PPM
 *     --shotevery N     save every N-th frame
 *     --out DIR         output directory
 *     --script FILE     input script: lines "frame joy2 bits" or
 *                       "frame key col row 0|1"
 *     --wav FILE        write audio (48 kHz mono)
 *     --hash            print a hash of RAM and framebuffer every frame
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runtime/lj.h"

static void log_cb(const char *m) { fprintf(stderr, "[lj] %s\n", m); }

typedef struct { int frame, kind, a, b, c; } event_t;

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void save_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", LJ_FB_W, LJ_FB_H);
    const uint8_t *fb = lj_framebuffer();
    for (int i = 0; i < LJ_FB_W * LJ_FB_H; i++) {
        uint32_t c = lj_palette[fb[i] & 15];
        uint8_t rgb[3] = {(uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void wav_header(FILE *f, uint32_t samples)
{
    uint32_t rate = 48000, bytes = samples * 2;
    fwrite("RIFF", 1, 4, f);
    uint32_t v = 36 + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    uint16_t s = 1; fwrite(&s, 2, 1, f);
    s = 1; fwrite(&s, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    v = rate * 2; fwrite(&v, 4, 1, f);
    s = 2; fwrite(&s, 2, 1, f);
    s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
}

extern int lj_debug_flags;
extern int lj_recomp_enabled;
const uint8_t *lj_debug_ram(void);
int lj_game_init(int sample_rate) __attribute__((weak));
uint32_t lj_debug_hash(void) __attribute__((weak));

int main(int argc, char **argv)
{
    const char *prg = NULL, *out = ".", *script = NULL, *wav = NULL, *dumpram = NULL;
    int entry = -1, frames = 100, game = 0, shotevery = 0, hash = 0;
    int shots[256], nshots = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--prg")) prg = argv[++i];
        else if (!strcmp(argv[i], "--entry")) entry = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--game")) game = 1;
        else if (!strcmp(argv[i], "--frames")) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out")) out = argv[++i];
        else if (!strcmp(argv[i], "--script")) script = argv[++i];
        else if (!strcmp(argv[i], "--wav")) wav = argv[++i];
        else if (!strcmp(argv[i], "--shotevery")) shotevery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hash")) hash = 1;
        else if (!strcmp(argv[i], "--dumpram")) dumpram = argv[++i];
        else if (!strcmp(argv[i], "--debug")) lj_debug_flags = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interp")) lj_recomp_enabled = 0;
        else if (!strcmp(argv[i], "--shot")) {
            char *s = argv[++i];
            for (char *t = strtok(s, ","); t && nshots < 256; t = strtok(NULL, ","))
                shots[nshots++] = atoi(t);
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }
    lj_set_log(log_cb);
    if (game) {
        if (!lj_game_init || lj_game_init(48000) != 0) {
            fprintf(stderr, "game init failed\n");
            return 1;
        }
    } else {
        if (!prg || entry < 0) {
            fprintf(stderr, "need --prg and --entry, or --game\n");
            return 2;
        }
        size_t len;
        uint8_t *d = read_file(prg, &len);
        if (!d) { fprintf(stderr, "cannot read %s\n", prg); return 1; }
        lj_power_on(48000);
        if (lj_load_prg(d, len)) { fprintf(stderr, "bad prg\n"); return 1; }
        lj_start((uint16_t)entry);
        free(d);
    }
    event_t ev[4096];
    int nev = 0;
    if (script) {
        FILE *f = fopen(script, "r");
        if (!f) { fprintf(stderr, "cannot read %s\n", script); return 1; }
        char line[256];
        while (fgets(line, sizeof line, f) && nev < 4096) {
            event_t e = {0};
            char kind[16];
            if (line[0] == '#' || line[0] == '\n') continue;
            int n = sscanf(line, "%d %15s %d %d %d", &e.frame, kind, &e.a, &e.b, &e.c);
            if (n < 3) continue;
            if (!strcmp(kind, "joy2")) e.kind = 2;
            else if (!strcmp(kind, "joy1")) e.kind = 1;
            else if (!strcmp(kind, "key")) e.kind = 3;
            else continue;
            ev[nev++] = e;
        }
        fclose(f);
    }
    FILE *wf = NULL;
    uint32_t nsamples = 0;
    if (wav) {
        wf = fopen(wav, "wb");
        if (wf) wav_header(wf, 0);
    }
    for (int fr = 0; fr < frames; fr++) {
        for (int i = 0; i < nev; i++) {
            if (ev[i].frame != fr) continue;
            if (ev[i].kind == 1 || ev[i].kind == 2) lj_set_joystick(ev[i].kind, (uint8_t)ev[i].a);
            else lj_set_key(ev[i].a, ev[i].b, ev[i].c);
        }
        lj_run_frame();
        int16_t buf[4096];
        size_t n;
        while ((n = lj_audio_read(buf, 4096)) > 0) {
            if (wf) { fwrite(buf, 2, n, wf); nsamples += (uint32_t)n; }
        }
        int want = shotevery > 0 && fr % shotevery == 0;
        for (int i = 0; i < nshots; i++) if (shots[i] == fr) want = 1;
        if (want) {
            char path[512];
            snprintf(path, sizeof path, "%s/f%05d.ppm", out, fr);
            save_ppm(path);
        }
        if (hash && lj_debug_hash) printf("frame %d hash %08x\n", fr, lj_debug_hash());
    }
    if (wf) {
        fseek(wf, 0, SEEK_SET);
        wav_header(wf, nsamples);
        fclose(wf);
    }
    if (dumpram) {
        FILE *f = fopen(dumpram, "wb");
        if (f) {
            fwrite(lj_debug_ram(), 1, 65536, f);
            fclose(f);
        }
    }
    fprintf(stderr, "done: %d frames, clock %llu\n", frames, (unsigned long long)lj_clock());
    return 0;
}
