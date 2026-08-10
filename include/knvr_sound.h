#ifndef KNVR_SOUND_H
#define KNVR_SOUND_H

/*
 * Sound events: the same 480-byte contract, a different subprocess.
 *
 * Its own detector rather than a second job for the object one, decided
 * deliberately.  They have different models, different frame rates and
 * different failure modes, and sharing a process would mean a wedged
 * audio model stopping the camera from seeing.
 *
 * Audio arrives as its own ffmpeg reading the same camera: -vn to s16le
 * mono on a pipe.  That is a second RTSP session, which the video path
 * carefully avoids - but it is only started for a camera that asked for
 * sound events, and an audio-only pull is a fraction of the video one.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One second at 16 kHz mono, which is what the small audio classifiers
 * are trained on and what makes the window self-framing. */
#define KNVR_SOUND_RATE 16000
#define KNVR_SOUND_WINDOW KNVR_SOUND_RATE
#define KNVR_SOUND_LABEL_MAX 32

typedef struct knvr_sound_event {
    int class_id;
    float score;
} knvr_sound_event;

typedef struct knvr_sound knvr_sound;

typedef struct knvr_sound_options {
    /* The detector command, argv-style and NULL-terminated; NULL uses the
     * bundled one on PATH. */
    const char *const *argv;
    /* ffmpeg's stderr sink.  Never the terminal. */
    const char *log_path;
    float min_score;
} knvr_sound_options;

void knvr_sound_options_init(knvr_sound_options *options);

bool knvr_sound_start(
    knvr_sound **sound, const char *url, const knvr_sound_options *options);
void knvr_sound_stop(knvr_sound *sound);
const char *knvr_sound_error(const knvr_sound *sound);

/*
 * Read the next window and classify it.  Returns false when no full
 * window is ready yet, which is ordinary - a second of audio takes a
 * second to arrive.
 */
bool knvr_sound_step(
    knvr_sound *sound, knvr_sound_event *events, size_t capacity,
    size_t *count);

/* The label for a sound class, or NULL outside the allowlist. */
const char *knvr_sound_label(int class_id);

#ifdef __cplusplus
}
#endif

#endif /* KNVR_SOUND_H */
