/* Replay decoded motion; keep the view's provider steps and outage state. */
#include "kilix_rtsp.h"
#include "kilix_motion_detect.h"
#include <string.h>
static uint8_t replay_pixels[640 * 360 * 4];
static void replay_stats(const krtsp_source *source, krtsp_source_stats *stats)
{ (void)source; memset(stats, 0, sizeof(*stats)); stats->frames = 1; }
static const uint8_t *replay_borrow(krtsp_source *source, int *age)
{ (void)source; (void)age; return replay_pixels; }
static void replay_release(krtsp_source *source) { (void)source; }
static size_t replay_motion(kmd_detector *detector, const uint8_t *frame,
                           kmd_box *boxes, size_t capacity, kmd_result *result)
{
    (void)detector; (void)frame; (void)capacity;
    memset(result, 0, sizeof(*result));
    memset(boxes, 0, sizeof(*boxes));
    boxes[0].x1 = 100; boxes[0].y1 = 100;
    return 1;
}
#define krtsp_source_get_stats replay_stats
#define krtsp_source_borrow replay_borrow
#define krtsp_source_release replay_release
#define kmd_detect replay_motion
#include "../src/knvr_view.c"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);return 1;} } while (0)
int main(void)
{
    feed at;
    ksd_options sound;
    kod_options object;
    knvr_store *store = NULL;
    const char *sound_args[] = {"python3", "tests/fixture_providers.py", "sound-fail", NULL};
    const char *object_args[] = {"python3", "tests/fixture_providers.py", "object-fail", NULL};
    memset(&at, 0, sizeof(at));
    strcpy(at.name, "synthetic-view");
    at.frame = replay_pixels;
    CHECK(knvr_store_open(&store, ":memory:"));
    ksd_options_init(&sound); sound.argv = sound_args; sound.timeout_seconds = 1;
    sound.log_path = "build/view-audio.log";
    CHECK(ksd_open(&at.sound, "build/integration.wav", &sound));
    kod_options_init(&object); object.argv = object_args; object.timeout_seconds = 1;
    CHECK(kod_open(&at.detector, &object));
    for (int i = 0; i < 300 && (!at.sound_failed || !at.detector_failed); ++i) {
        const struct timespec pause = {0, 10000000};
        (void)feed_step(&at, store);
        (void)nanosleep(&pause, NULL);
    }
    CHECK(at.sound_failed && at.detector_failed);
    CHECK(at.sound == NULL && at.detector == NULL && at.box_count == 0);
    for (size_t i = 0; i < KSD_CLASS_COUNT; ++i) CHECK(at.scores[i] == 0.0f);
    knvr_store_close(store);
    puts("ok view retains explicit outages for both providers");
    return 0;
}
