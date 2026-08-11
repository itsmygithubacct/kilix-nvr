/*
 * The sound path, end to end, without a camera.
 *
 * ffmpeg does not care whether its input is an RTSP url or a file, which
 * makes a wav on disk a complete stand-in for a camera's audio track:
 * the same ffmpeg, the same s16le pipe, the same windows, the same
 * classifier subprocess.  So this exercises the whole path and needs
 * nothing running.
 *
 * With no arguments it uses the fake listener, so `make test` is
 * deterministic and needs no model.  Pointed at the real one it becomes
 * the audition:
 *
 *   ./build/test-sound --audio speech.wav -- \
 *       /path/to/venv/bin/python tools/kilix-nvr-listen
 */

#include "knvr_sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

#define WAV "build/test-sound.wav"

static const char *const FAKE[] = {"python3", "tests/fake_listen.py", NULL};
static const char *audio_path = WAV;
static const char *const *listener = FAKE;

static void put32(FILE *file, uint32_t value)
{
    (void)fputc((int)(value & 0xFFu), file);
    (void)fputc((int)((value >> 8) & 0xFFu), file);
    (void)fputc((int)((value >> 16) & 0xFFu), file);
    (void)fputc((int)((value >> 24) & 0xFFu), file);
}

static void put16(FILE *file, uint16_t value)
{
    (void)fputc((int)(value & 0xFFu), file);
    (void)fputc((int)((value >> 8) & 0xFFu), file);
}

/* Four seconds of quiet noise at 16 kHz mono: enough windows to prove the
 * pipe stays in step, and nothing a model would call anything. */
static bool write_wav(const char *path)
{
    const uint32_t samples = KNVR_SOUND_RATE * 4u;
    const uint32_t bytes = samples * 2u;
    FILE *file = fopen(path, "wb");
    uint32_t state = 12345u;

    if (file == NULL) {
        return false;
    }
    (void)fputs("RIFF", file);
    put32(file, 36u + bytes);
    (void)fputs("WAVEfmt ", file);
    put32(file, 16u);
    put16(file, 1u);                        /* pcm */
    put16(file, 1u);                        /* mono */
    put32(file, (uint32_t)KNVR_SOUND_RATE);
    put32(file, (uint32_t)KNVR_SOUND_RATE * 2u);
    put16(file, 2u);
    put16(file, 16u);
    (void)fputs("data", file);
    put32(file, bytes);
    for (uint32_t i = 0u; i < samples; i++) {
        int16_t value;

        state = state * 1103515245u + 12345u;
        value = (int16_t)((int)((state >> 16) & 0x1FFu) - 256);
        (void)fputc(value & 0xFF, file);
        (void)fputc((value >> 8) & 0xFF, file);
    }
    return fclose(file) == 0;
}

static bool listen(size_t *windows, size_t *heard_total, int *class_id,
                   float *best)
{
    knvr_sound_options options;
    knvr_sound *sound = NULL;
    const time_t deadline = time(NULL) + 30;

    *windows = 0u;
    *heard_total = 0u;
    *class_id = -1;
    *best = 0.0f;

    knvr_sound_options_init(&options);
    options.argv = listener;
    options.min_score = 0.25f;
    options.log_path = "build/test-sound-ffmpeg.log";
    if (!knvr_sound_start(&sound, audio_path, &options)) {
        return false;
    }
    while (time(NULL) < deadline) {
        knvr_sound_event events[8];
        size_t count = 0u;
        struct timespec pause = {0, 50 * 1000 * 1000};

        if (!knvr_sound_step(sound, events, 8u, &count)) {
            if (knvr_sound_error(sound) != NULL) {
                break;   /* the file ran out, which is how this ends */
            }
            (void)nanosleep(&pause, NULL);
            continue;
        }
        (*windows)++;
        *heard_total += count;
        for (size_t i = 0u; i < count; i++) {
            if (events[i].score > *best) {
                *best = events[i].score;
                *class_id = events[i].class_id;
            }
        }
    }
    knvr_sound_stop(sound);
    return true;
}

static bool test_audio_becomes_sound_events(void)
{
    size_t windows = 0u;
    size_t heard = 0u;
    int class_id = -1;
    float best = 0.0f;

    CHECK(listen(&windows, &heard, &class_id, &best));
    /* Four seconds of audio is three or four whole windows; the last
     * partial one is not a window and is not reported. */
    CHECK(windows >= 3u);
    CHECK(heard >= windows);
    CHECK(class_id >= 0);
    CHECK(knvr_sound_label(class_id) != NULL);
    (void)printf("   %zu windows, best %s %.2f\n", windows,
                 knvr_sound_label(class_id), (double)best);
    return true;
}

/* The allowlist is what makes a class id mean something. */
static bool test_the_allowlist_names_what_it_knows(void)
{
    CHECK(strcmp(knvr_sound_label(0), "speech") == 0);
    CHECK(strcmp(knvr_sound_label(6), "gunshot") == 0);
    CHECK(knvr_sound_label(9) == NULL);
    CHECK(knvr_sound_label(-1) == NULL);
    CHECK(knvr_sound_label(1000) == NULL);
    return true;
}

static bool test_rejections(void)
{
    knvr_sound *sound = NULL;
    knvr_sound_options options;

    knvr_sound_options_init(&options);
    CHECK(!knvr_sound_start(NULL, audio_path, &options));
    CHECK(!knvr_sound_start(&sound, NULL, &options));
    CHECK(!knvr_sound_start(&sound, "", &options));
    CHECK(!knvr_sound_step(NULL, NULL, 0u, NULL));
    CHECK(knvr_sound_error(NULL) == NULL);
    knvr_sound_stop(NULL);
    return true;
}

int main(int argc, char **argv)
{
    const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        {"audio becomes sound events", test_audio_becomes_sound_events},
        {"the allowlist names what it knows",
         test_the_allowlist_names_what_it_knows},
        {"rejections", test_rejections}
    };
    size_t passed = 0u;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--audio") == 0 && i + 1 < argc) {
            audio_path = argv[++i];
        } else if (strcmp(argv[i], "--") == 0 && i + 1 < argc) {
            listener = (const char *const *)&argv[i + 1];
            break;
        } else {
            (void)fprintf(stderr,
                          "usage: test-sound [--audio FILE] [-- listener "
                          "argv...]\n");
            return 2;
        }
    }
    if (strcmp(audio_path, WAV) == 0 && !write_wav(WAV)) {
        (void)fprintf(stderr, "cannot write %s\n", WAV);
        return 1;
    }
    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
