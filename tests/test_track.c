#include "knvr_track.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

static knvr_detection_box box(int class_id, int x, int y, int w, int h,
                              float score)
{
    knvr_detection_box out;

    (void)memset(&out, 0, sizeof(out));
    out.class_id = class_id;
    out.score = score;
    out.x = x;
    out.y = y;
    out.w = w;
    out.h = h;
    return out;
}

static bool make(knvr_tracker **tracker, int min_hits)
{
    knvr_tracker_options options;

    knvr_tracker_options_init(&options);
    options.min_hits = min_hits;
    return knvr_tracker_create(tracker, &options);
}

/* The whole point: the same object across frames keeps one id. */
static bool test_one_object_keeps_its_id(void)
{
    knvr_tracker *tracker = NULL;
    int64_t first;

    CHECK(make(&tracker, 2));
    {
        const knvr_detection_box seen = box(0, 100, 100, 40, 80, 0.9f);

        CHECK(knvr_tracker_update(tracker, &seen, 1u, 1000));
        first = knvr_tracker_assigned(tracker, 0u);
        CHECK(first != 0);
    }
    /* Walking right, ten pixels a frame: overlap stays high. */
    for (int step = 1; step <= 8; step++) {
        const knvr_detection_box seen =
            box(0, 100 + step * 10, 100, 40, 80, 0.9f);

        CHECK(knvr_tracker_update(tracker, &seen, 1u, 1000 + step * 200));
        CHECK(knvr_tracker_assigned(tracker, 0u) == first);
    }
    CHECK(knvr_tracker_count(tracker) == 1u);
    CHECK(knvr_tracker_total(tracker) == 1);
    {
        const knvr_track *track = knvr_tracker_at(tracker, 0u);

        CHECK(track != NULL);
        CHECK(track->confirmed);
        CHECK(track->hits == 9);
        CHECK(track->travelled == 80);
        CHECK(!track->stationary);
    }
    knvr_tracker_free(tracker);
    return true;
}

/* Two people in one frame are two objects, and stay two. */
static bool test_two_objects_stay_apart(void)
{
    knvr_tracker *tracker = NULL;
    int64_t left;
    int64_t right;

    CHECK(make(&tracker, 1));
    {
        knvr_detection_box seen[2];

        seen[0] = box(0, 10, 100, 40, 80, 0.9f);
        seen[1] = box(0, 400, 100, 40, 80, 0.8f);
        CHECK(knvr_tracker_update(tracker, seen, 2u, 1000));
        left = knvr_tracker_assigned(tracker, 0u);
        right = knvr_tracker_assigned(tracker, 1u);
        CHECK(left != 0 && right != 0 && left != right);
    }
    {
        /* Reported in the other order this time, which a tracker keyed on
         * position must not care about. */
        knvr_detection_box seen[2];

        seen[0] = box(0, 405, 100, 40, 80, 0.8f);
        seen[1] = box(0, 15, 100, 40, 80, 0.9f);
        CHECK(knvr_tracker_update(tracker, seen, 2u, 1300));
        CHECK(knvr_tracker_assigned(tracker, 0u) == right);
        CHECK(knvr_tracker_assigned(tracker, 1u) == left);
    }
    CHECK(knvr_tracker_count(tracker) == 2u);
    knvr_tracker_free(tracker);
    return true;
}

/* A car is not a person, however well the boxes line up. */
static bool test_class_is_not_crossed(void)
{
    knvr_tracker *tracker = NULL;
    int64_t person;
    int64_t car;

    CHECK(make(&tracker, 1));
    {
        const knvr_detection_box seen = box(0, 100, 100, 40, 80, 0.9f);

        CHECK(knvr_tracker_update(tracker, &seen, 1u, 1000));
        person = knvr_tracker_assigned(tracker, 0u);
    }
    {
        const knvr_detection_box seen = box(2, 100, 100, 40, 80, 0.9f);

        CHECK(knvr_tracker_update(tracker, &seen, 1u, 1200));
        car = knvr_tracker_assigned(tracker, 0u);
    }
    CHECK(person != car);
    knvr_tracker_free(tracker);
    return true;
}

/*
 * A subject walking towards the camera grows fast; two consecutive boxes
 * can overlap by less than the threshold while plainly being the same
 * thing.  The centroid gate is what stops that becoming a new object
 * every second.
 */
static bool test_a_grower_is_not_a_newcomer(void)
{
    knvr_tracker *tracker = NULL;
    int64_t first;
    const knvr_detection_box small = box(0, 300, 200, 20, 40, 0.7f);
    const knvr_detection_box large = box(0, 292, 176, 36, 88, 0.8f);

    CHECK(make(&tracker, 1));
    CHECK(knvr_tracker_update(tracker, &small, 1u, 1000));
    first = knvr_tracker_assigned(tracker, 0u);
    CHECK(knvr_tracker_update(tracker, &large, 1u, 1500));
    CHECK(knvr_tracker_assigned(tracker, 0u) == first);
    CHECK(knvr_tracker_total(tracker) == 1);
    knvr_tracker_free(tracker);
    return true;
}

/* Gone for longer than the gap is gone: the next one is a new object. */
static bool test_a_long_absence_ends_a_track(void)
{
    knvr_tracker *tracker = NULL;
    knvr_tracker_options options;
    int64_t first;
    const knvr_detection_box seen = box(0, 100, 100, 40, 80, 0.9f);

    knvr_tracker_options_init(&options);
    options.max_gap_ms = 2000;
    CHECK(knvr_tracker_create(&tracker, &options));
    CHECK(knvr_tracker_update(tracker, &seen, 1u, 1000));
    first = knvr_tracker_assigned(tracker, 0u);

    CHECK(knvr_tracker_update(tracker, NULL, 0u, 2500));
    CHECK(knvr_tracker_count(tracker) == 1u);   /* still within the gap */
    CHECK(knvr_tracker_update(tracker, NULL, 0u, 4000));
    CHECK(knvr_tracker_count(tracker) == 0u);

    CHECK(knvr_tracker_update(tracker, &seen, 1u, 4200));
    CHECK(knvr_tracker_assigned(tracker, 0u) != first);
    CHECK(knvr_tracker_total(tracker) == 2);
    knvr_tracker_free(tracker);
    return true;
}

/* A parked car has to stop being news. */
static bool test_a_parked_car_settles(void)
{
    knvr_tracker *tracker = NULL;
    knvr_tracker_options options;
    const knvr_detection_box parked = box(2, 200, 300, 120, 90, 0.85f);

    knvr_tracker_options_init(&options);
    options.min_hits = 1;
    options.stationary_ms = 5000;
    options.stationary_px = 8;
    options.max_gap_ms = 60000;
    CHECK(knvr_tracker_create(&tracker, &options));
    for (int64_t at = 1000; at <= 4000; at += 1000) {
        CHECK(knvr_tracker_update(tracker, &parked, 1u, at));
        CHECK(!knvr_tracker_at(tracker, 0u)->stationary);
    }
    CHECK(knvr_tracker_update(tracker, &parked, 1u, 6500));
    CHECK(knvr_tracker_at(tracker, 0u)->stationary);

    /* And driving away un-parks it. */
    {
        const knvr_detection_box moving = box(2, 260, 300, 120, 90, 0.85f);

        CHECK(knvr_tracker_update(tracker, &moving, 1u, 7000));
        CHECK(!knvr_tracker_at(tracker, 0u)->stationary);
    }
    knvr_tracker_free(tracker);
    return true;
}

/* One frame is not an object. */
static bool test_a_single_sighting_is_not_confirmed(void)
{
    knvr_tracker *tracker = NULL;
    const knvr_detection_box seen = box(0, 100, 100, 40, 80, 0.9f);

    CHECK(make(&tracker, 3));
    CHECK(knvr_tracker_update(tracker, &seen, 1u, 1000));
    CHECK(!knvr_tracker_at(tracker, 0u)->confirmed);
    CHECK(knvr_tracker_update(tracker, &seen, 1u, 1200));
    CHECK(!knvr_tracker_at(tracker, 0u)->confirmed);
    CHECK(knvr_tracker_update(tracker, &seen, 1u, 1400));
    CHECK(knvr_tracker_at(tracker, 0u)->confirmed);
    knvr_tracker_free(tracker);
    return true;
}

/*
 * A full tracker keeps what it knows.
 *
 * More detections than slots is the case that matters, and the newcomer
 * is refused rather than a confirmed object evicted to make room for what
 * may be a hallucination.  Spaced far enough apart that neither overlap
 * nor the centroid gate joins them, so each really is its own object.
 */
static bool test_a_crowd_does_not_evict(void)
{
    knvr_tracker *tracker = NULL;
    knvr_detection_box seen[KNVR_TRACK_MAX + 4];
    const size_t crowd = sizeof(seen) / sizeof(seen[0]);
    int64_t first;

    CHECK(make(&tracker, 1));
    for (size_t i = 0u; i < crowd; i++) {
        seen[i] = box(0, (int)i * 60, 100, 20, 40, 0.5f);
    }
    CHECK(knvr_tracker_update(tracker, seen, crowd, 1000));
    first = knvr_tracker_assigned(tracker, 0u);
    CHECK(first != 0);
    /* Every slot, and not one more. */
    CHECK(knvr_tracker_count(tracker) == KNVR_TRACK_MAX);
    for (int round = 0; round < 3; round++) {
        CHECK(knvr_tracker_update(tracker, seen, crowd,
                                  1200 + round * 200));
        CHECK(knvr_tracker_assigned(tracker, 0u) == first);
        CHECK(knvr_tracker_count(tracker) == KNVR_TRACK_MAX);
    }
    /* The ones that did not fit were never tracked, rather than tracked
     * and then quietly dropped. */
    CHECK(knvr_tracker_total(tracker) == KNVR_TRACK_MAX);
    knvr_tracker_free(tracker);
    return true;
}

/* Drawing must stay inside the buffer whatever the boxes say. */
static bool test_drawing_clips(void)
{
    knvr_tracker *tracker = NULL;
    uint8_t frame[16 * 12 * 4];
    const knvr_detection_box outside = box(0, -40, -30, 200, 200, 0.9f);

    (void)memset(frame, 0, sizeof(frame));
    CHECK(make(&tracker, 1));
    CHECK(knvr_tracker_update(tracker, &outside, 1u, 1000));
    knvr_track_draw(frame, 16, 12, tracker);
    knvr_track_draw(NULL, 16, 12, tracker);
    knvr_track_draw(frame, 0, 0, tracker);
    knvr_tracker_free(tracker);
    return true;
}

int main(void)
{
    const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        {"one object keeps its id", test_one_object_keeps_its_id},
        {"two objects stay apart", test_two_objects_stay_apart},
        {"class is not crossed", test_class_is_not_crossed},
        {"a grower is not a newcomer", test_a_grower_is_not_a_newcomer},
        {"a long absence ends a track", test_a_long_absence_ends_a_track},
        {"a parked car settles", test_a_parked_car_settles},
        {"a single sighting is not confirmed",
         test_a_single_sighting_is_not_confirmed},
        {"a crowd does not evict", test_a_crowd_does_not_evict},
        {"drawing clips", test_drawing_clips}
    };
    size_t passed = 0u;

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
