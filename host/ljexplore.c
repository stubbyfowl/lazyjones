/*
 * ljexplore.c - coverage guided exploration of the game for the recompiler.
 *
 * Runs the game in the interpreter with tracing enabled. From a growing
 * corpus of saved machine states it plays short segments with random
 * joystick and key input and keeps the end states of segments that reached
 * new code. The result is a trace file (see runtime/trace.h) that the
 * recompiler uses to find code, self modifying code and entry points.
 *
 *   ljexplore --prg FILE --entry HEX --seconds N --out DIR [--bootref FILE]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../runtime/c64.h"
#include "../runtime/trace.h"

static void log_cb(const char *m) { (void)m; }

static uint64_t rng = 88172645463325252ULL;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

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

static unsigned coverage(void)
{
    unsigned n = 0;
    for (unsigned a = 0; a < 65536; a++)
        if ((lj_trace->flags[a] & TR_EXEC) && !(lj_trace->flags[a] & TR_ROM))
            n++;
    return n;
}

#define MAXCORPUS 600

int main(int argc, char **argv)
{
    const char *prg = NULL, *out = ".", *bootref = NULL;
    int entry = -1;
    double seconds = 60;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--prg")) prg = argv[++i];
        else if (!strcmp(argv[i], "--entry")) entry = (int)strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out")) out = argv[++i];
        else if (!strcmp(argv[i], "--bootref")) bootref = argv[++i];
        else if (!strcmp(argv[i], "--seed")) rng = strtoull(argv[++i], NULL, 10) * 2654435761ULL + 1;
    }
    if (!prg || entry < 0) {
        fprintf(stderr, "need --prg and --entry\n");
        return 2;
    }
    lj_set_log(log_cb);
    size_t len;
    uint8_t *d = read_file(prg, &len);
    if (!d) { perror(prg); return 1; }
    lj_trace = calloc(1, sizeof *lj_trace);
    lj_power_on(48000);
    lj_load_prg(d, len);
    lj_start((uint16_t)entry);
    uint16_t la = (uint16_t)(d[0] | (d[1] << 8));
    for (size_t i = 0; i + 2 < len; i++)
        lj_trace->known[(uint16_t)(la + i)] = 1;
    for (unsigned a = 0x0400; a < 0x0800; a++)
        lj_trace->known[a] = 1;
    if (bootref) {
        size_t rl;
        uint8_t *ref = read_file(bootref, &rl);
        if (ref && rl == 65536) {
            for (unsigned a = 0; a < 0x400; a++)
                if (C.ram[a] == ref[a])
                    lj_trace->known[a] = 1;
        }
        free(ref);
    } else {
        for (unsigned a = 0; a < 0x400; a++)
            lj_trace->known[a] = 1;
    }

    size_t ssize = lj_state_size();
    uint8_t **corpus = calloc(MAXCORPUS, sizeof *corpus);
    int ncorpus = 0;
    corpus[ncorpus] = malloc(ssize);
    lj_state_save(corpus[ncorpus++]);

    unsigned cov = coverage();
    time_t t0 = time(NULL);
    long iter = 0, frames_total = 0;
    int shots = 0;
    while (difftime(time(NULL), t0) < seconds) {
        int pick;
        if (ncorpus > 4 && (rnd() % 3) == 0)
            pick = ncorpus - 1 - (int)(rnd() % 4);
        else
            pick = (int)(rnd() % (unsigned)ncorpus);
        lj_state_load(corpus[pick], ssize);
        int seg = 150 + (int)(rnd() % 400);
        int f = 0;
        while (f < seg) {
            uint8_t joy = 0;
            unsigned r = rnd() % 16;
            static const uint8_t dirs[] = {0, 1, 2, 4, 8, 5, 9, 6, 10};
            joy = dirs[r % 9];
            if (rnd() % 3 == 0)
                joy |= 0x10;
            int hold = 3 + (int)(rnd() % 60);
            lj_set_joystick(2, joy);
            lj_clear_keys();
            if (rnd() % 40 == 0) {
                static const int keys[][2] = {{0, 1}, {0, 4}, {7, 4}, {7, 0}, {7, 3}, {1, 0}, {1, 3},
                                              {2, 0}, {2, 3}, {3, 0}, {3, 3}, {4, 0}, {4, 3}, {7, 7}};
                int k = (int)(rnd() % 14);
                lj_set_key(keys[k][0], keys[k][1], 1);
            }
            for (int h = 0; h < hold && f < seg; h++, f++) {
                lj_run_frame();
                int16_t buf[2048];
                while (lj_audio_read(buf, 2048) > 0) {}
            }
        }
        frames_total += seg;
        lj_set_joystick(2, 0);
        lj_clear_keys();
        unsigned c = coverage();
        if (c > cov) {
            cov = c;
            uint8_t *st;
            if (ncorpus < MAXCORPUS) {
                st = corpus[ncorpus++] = malloc(ssize);
            } else {
                st = corpus[1 + rnd() % (MAXCORPUS - 1)];
            }
            lj_state_save(st);
            char path[512];
            snprintf(path, sizeof path, "%s/cov%04d.ppm", out, shots++);
            save_ppm(path);
            fprintf(stderr, "[%5.0fs] iter %ld frames %ld coverage %u corpus %d\n",
                    difftime(time(NULL), t0), iter, frames_total, cov, ncorpus);
        }
        iter++;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/trace.bin", out);
    FILE *tf = fopen(path, "wb");
    fwrite(lj_trace, sizeof *lj_trace, 1, tf);
    fclose(tf);
    fprintf(stderr, "done: %ld segments, %ld frames, coverage %u, instructions %llu\n",
            iter, frames_total, cov, (unsigned long long)lj_trace->instructions);
    return 0;
}
