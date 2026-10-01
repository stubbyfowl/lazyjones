/*
 * audio.h - sound output for Android.
 *
 * AAudio (Android 9 and newer, loaded at run time) or the Java AudioTrack
 * (through JNI) on older systems. The game thread pushes 16 bit mono
 * samples into a ring buffer; the audio thread or callback takes them out.
 */
#ifndef LJ_AUDIO_H
#define LJ_AUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <jni.h>

/* Open the output. Returns the sample rate, or 0 when no output works. */
int audio_open(JavaVM *vm, int sdk);
void audio_close(void);
void audio_pause(void);
void audio_resume(void);
/* Queue samples (drops what does not fit). */
void audio_write(const int16_t *s, size_t n);
/* Samples waiting to be played (ring buffer only). */
size_t audio_queued(void);
/* 1 when the device was lost (for example headphones unplugged) and the
 * stream must be opened again. */
int audio_lost(void);

#endif
