/* The real NVR runner with only clock and decoded-motion input replayed.
 * Both provider subprocesses, C wire readers, event store, tracker and zones
 * are production code. No camera, alarm, service or persistent configuration. */
#include "knvr_watch.h"
#include <time.h>
#include <string.h>
static time_t clock_value = 100;
static size_t motion_count;
static uint8_t pixels[320 * 320 * 4];
static bool replay_step(knvr_watch *watch, const uint8_t **frame,
                        knvr_box *boxes, size_t capacity, size_t *count)
{
    (void)watch;
    *frame = pixels;
    *count = motion_count < capacity ? motion_count : capacity;
    if (*count) boxes[0] = (knvr_box){20, 20, 100, 100};
    return true;
}
static int replay_size(const knvr_watch *watch) { (void)watch; return 320; }
static void replay_stats(const knvr_watch *watch, knvr_watch_stats *stats)
{ (void)watch; (void)memset(stats, 0, sizeof(*stats)); }
static time_t replay_time(time_t *out)
{ if (out != NULL) *out = clock_value; return clock_value; }
#define knvr_watch_step replay_step
#define knvr_watch_width replay_size
#define knvr_watch_height replay_size
#define knvr_watch_get_stats replay_stats
#define time replay_time
#include "../src/knvr_run.c"
#undef time
#include "kilix_mask.h"
#include <math.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); return false; } } while (0)
static void pause_step(void) { const struct timespec t = {0, 10000000}; nanosleep(&t, NULL); }
static bool open_sound(camera_run *run, const char *mode, float fallback)
{
    const char *args[] = {"python3", "tests/fixture_providers.py", mode, NULL};
    ksd_options opts;
    ksd_options_init(&opts);
    opts.argv = args;
    opts.min_score = fallback;
    opts.timeout_seconds = 1;
    opts.log_path = "build/integration-audio.log";
    return ksd_open(&run->sound, "build/integration.wav", &opts);
}
static bool one_window(camera_run *run, knvr_store *store)
{
    uint64_t before = ksd_windows(run->sound);
    for (int i = 0; i < 300; ++i) {
        listen(run, store, false);
        if (run->sound == NULL) return false;
        if (ksd_windows(run->sound) > before) return true;
        pause_step();
    }
    return false;
}
static void init_run(camera_run *run)
{
    memset(run, 0, sizeof(*run));
    strcpy(run->name, "synthetic-consumer");
    run->policy.record = KNVR_RECORD_OFF;
    run->watch = (knvr_watch *)pixels; /* opaque token, used only by replay */
    run->offered = pixels;
    run->detects = true;
    clock_value = 100;
    motion_count = 0;
}
static bool collect_boxes(kod_detector *detector, kod_box *boxes, size_t *count)
{
    bool done = false;
    for (int i = 0; i < 300 && !done; ++i) {
        CHECK(kod_take(detector, boxes, KOD_BOX_MAX, count, &done));
        if (!done) pause_step();
    }
    return done;
}
static bool shared_consumers(void)
{
    camera_run run;
    knvr_store *store = NULL;
    kod_detector *detector = NULL;
    kod_options opts;
    knvr_run_options runopts;
    knvr_detection rows[32];
    knvr_object objects[4];
    size_t count = 0;
    bool offered = false;
    const char *args[] = {"python3", "tests/fixture_providers.py", "object", NULL};
    kmask *mask = NULL;
    init_run(&run);
    CHECK(knvr_store_open(&store, ":memory:"));
    CHECK(knvr_tracker_create(&run.tracker, NULL));
    kod_options_init(&opts); opts.argv = args; opts.timeout_seconds = 2;
    CHECK(kod_open(&detector, &opts));
    knvr_run_options_init(&runopts);
    camera_step(&run, detector, store, &runopts, &offered);
    CHECK(!offered && run.event_id == 0 && !kod_busy(detector));
    CHECK(kmask_create(&mask, 320, 320, 4));
    kmask_fill_rect(mask, 0, 0, 320, 320, 1);
    CHECK(kmask_region_set_attr(mask, 1, "preclusive", "yes"));
    CHECK(kmask_save(mask, "build/integration-zones.png"));
    kmask_free(mask);
    CHECK(knvr_zones_load(&run.zones, "build/integration-zones.png", 320, 320));
    motion_count = 1;
    camera_step(&run, detector, store, &runopts, &offered);
    CHECK(!offered && run.event_id == 0 && run.suppressed == 1);
    knvr_zones_free(run.zones); run.zones = NULL;
    for (int pass = 0; pass < 2; ++pass) {
        kod_box seen[KOD_BOX_MAX];
        offered = false;
        camera_step(&run, detector, store, &runopts, &offered);
        CHECK(offered && run.event_id != 0);
        CHECK(collect_boxes(detector, seen, &count));
        CHECK(count == 1 && seen[0].class_id == 16 && seen[0].score == .75f);
        apply_detections(&run, store, seen, count, clock_value, false);
    }
    CHECK(knvr_store_objects(store, run.event_id, objects, 4, &count));
    CHECK(count == 1 && strcmp(objects[0].label, "dog") == 0);
    /* Same class/box from two overlapping crops is still one detection. */
    {
        kod_rect crops[] = {{0, 0, 320, 320}, {0, 0, 320, 320}};
        kod_box seen[KOD_BOX_MAX];
        CHECK(kod_detect_regions(detector, pixels, 320, 320, crops, 2, seen, KOD_BOX_MAX, &count));
        CHECK(count == 1);
    }
    printf("ok motion gate, preclusive zone, object allowlist, crop and track deduplication\n");
    CHECK(open_sound(&run, "sound", .5f));
    CHECK(one_window(&run, store));
    CHECK(knvr_store_detections(store, run.event_id, rows, 32, &count));
    printf("shared event: %zu detections (two object samples plus nine sounds)\n", count);
    CHECK(count == 11);
    for (int id = 0; id < KSD_CLASS_COUNT; ++id) {
        bool found = false;
        for (size_t j = 0; j < count; ++j)
            if (!strcmp(rows[j].label, ksd_label(id)) && rows[j].w == 0) found = true;
        CHECK(found);
    }
    ksd_close(run.sound); kod_close(detector);
    knvr_tracker_free(run.tracker); knvr_store_close(store);
    return true;
}
static bool quiet_interval(void)
{
    camera_run run;
    knvr_store *store = NULL;
    knvr_run_options opts;
    bool offered = false;
    int64_t event;
    init_run(&run);
    CHECK(knvr_store_open(&store, ":memory:"));
    CHECK(open_sound(&run, "sound", .5f));
    CHECK(one_window(&run, store));
    event = run.event_id;
    CHECK(event > 0 && run.last_motion == 100);
    clock_value = 104;
    CHECK(one_window(&run, store));
    CHECK(run.event_id == event);
    CHECK(run.last_motion == 104);
    ksd_close(run.sound); run.sound = NULL;
    knvr_run_options_init(&opts);
    clock_value = 108;
    camera_step(&run, NULL, store, &opts, &offered);
    CHECK(run.event_id == event);
    clock_value = 109;
    camera_step(&run, NULL, store, &opts, &offered);
    CHECK(run.event_id == 0);
    knvr_store_close(store);
    return true;
}
static bool sound_policy(void)
{
    camera_run run;
    ksd_event events[KSD_CLASS_COUNT];
    size_t count = 0;
    bool ready = false;
    init_run(&run);
    CHECK(setenv("KILIX_SOUND_THRESHOLDS", "build/thresholds.txt", 1) == 0);
    CHECK(open_sound(&run, "policy", .6f));
    for (int i = 0; i < 300 && !ready; ++i) {
        ready = ksd_step(run.sound, events, KSD_CLASS_COUNT, &count);
        if (!ready) pause_step();
    }
    CHECK(ready && count == 1 && events[0].class_id == KSD_KNOCK);
    for (int i = 0; i < KSD_CLASS_COUNT; ++i) {
        CHECK(ksd_scores(run.sound)[i] == .5f);
        printf("policy %s %.9g %.9g\n", ksd_label(i), (double)ksd_scores(run.sound)[i], (double)ksd_threshold(run.sound, i));
    }
    ksd_close(run.sound);
    unsetenv("KILIX_SOUND_THRESHOLDS");
    return true;
}
static bool provider_failure(void)
{
    camera_run run;
    knvr_store *store = NULL;
    init_run(&run);
    CHECK(knvr_store_open(&store, ":memory:"));
    CHECK(open_sound(&run, "sound-fail", .5f));
    CHECK(!one_window(&run, store));
    CHECK(run.sound == NULL && run.sound_restarts == 1 && run.event_id == 0);
    CHECK(run.sound_retry_at == clock_value + SOUND_RETRY_MIN);
    {
        kod_detector *detector = NULL;
        kod_options opts;
        kod_box seen[KOD_BOX_MAX];
        size_t count = 99;
        const char *args[] = {"python3", "tests/fixture_providers.py", "object-fail", NULL};
        kod_options_init(&opts); opts.argv = args; opts.timeout_seconds = 1;
        CHECK(kod_open(&detector, &opts));
        CHECK(!kod_detect(detector, pixels, 320, 320, seen, KOD_BOX_MAX, &count));
        CHECK(kod_error(detector) != NULL && count == 0);
        kod_close(detector);
    }
    knvr_store_close(store);
    return true;
}
int main(void)
{
    int failed = 0;
    const struct { const char *name; bool (*fn)(void); } tests[] = {
        {"shared consumer and event capacity", shared_consumers},
        {"sound refreshes event quiet interval", quiet_interval},
        {"raw scores and per-event policy", sound_policy},
        {"provider failure is distinct from negative detection", provider_failure}
    };
    for (size_t i = 0; i < sizeof(tests)/sizeof(tests[0]); ++i) {
        bool ok = tests[i].fn();
        printf("%s %s\n", ok ? "ok" : "FAIL", tests[i].name);
        if (!ok) ++failed;
    }
    return failed ? 1 : 0;
}
