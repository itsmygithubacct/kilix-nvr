#include "knvr_watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

#define FRAME_W 64
#define FRAME_H 48

static size_t count_box_pixels(const uint8_t *frame)
{
    size_t hits = 0u;

    for (int i = 0; i < FRAME_W * FRAME_H; i++) {
        if (frame[i * 4] == 0x40 && frame[i * 4 + 1] == 0xFF &&
            frame[i * 4 + 2] == 0x40) {
            hits++;
        }
    }
    return hits;
}

/*
 * The defaults are measured, not chosen: 320 wide loses subjects under
 * about 50 px, so the substream shape is what a camera decodes at.
 */
static bool
test_defaults_are_the_measured_ones(void)
{
    knvr_watch_options options;

    knvr_watch_options_init(&options);
    CHECK(options.width == 640 && options.height == 360);
    CHECK(options.mask_path == NULL);
    CHECK(options.log_path == NULL);   /* never the terminal */
    knvr_watch_options_init(NULL);
    return true;
}

/*
 * A box draws its outline and nothing else.  Checked by counting rather
 * than by eye, because "the overlay looks right" is how an off-by-one on
 * the far edge survives.
 */
static bool
test_boxes_draw_their_outline(void)
{
    uint8_t *frame = calloc((size_t)FRAME_W * FRAME_H * 4u, 1u);
    knvr_box box = {10, 8, 20, 16};

    CHECK(frame != NULL);
    knvr_watch_draw_boxes(frame, FRAME_W, FRAME_H, &box, 1u);

    /* Perimeter of a 20x16 rectangle, corners counted once. */
    CHECK(count_box_pixels(frame) == 2u * 20u + 2u * 16u - 4u);

    /* The corners are on it and the interior is untouched. */
    {
        const uint8_t *corner = frame + ((size_t)8 * FRAME_W + 10u) * 4u;
        const uint8_t *inside = frame + ((size_t)12 * FRAME_W + 15u) * 4u;

        CHECK(corner[1] == 0xFF);
        CHECK(inside[0] == 0 && inside[1] == 0 && inside[2] == 0);
    }
    free(frame);
    return true;
}

/*
 * Boxes come from kmd_detect() already in source coordinates - the module
 * says so explicitly - so anything at the frame edge must survive being
 * drawn without running off the end of the buffer.  Rescaling them was
 * this file's first bug and it produced negative heights.
 */
static bool
test_edges_and_degenerate_boxes(void)
{
    uint8_t *frame = calloc((size_t)FRAME_W * FRAME_H * 4u, 1u);
    const knvr_box boxes[] = {
        {0, 0, FRAME_W, FRAME_H},         /* exactly the frame */
        {FRAME_W - 2, FRAME_H - 2, 5, 5}, /* running off the corner */
        {-4, -4, 8, 8},                   /* starting off the top left */
        {10, 10, 0, 5},                   /* no width */
        {10, 10, 5, 0},                   /* no height */
        {10, 10, -3, -3}                  /* negative, as the bug produced */
    };

    CHECK(frame != NULL);
    knvr_watch_draw_boxes(frame, FRAME_W, FRAME_H, boxes,
                          sizeof(boxes) / sizeof(boxes[0]));
    /* The point is that it returns at all; ASan is the real assertion. */
    CHECK(count_box_pixels(frame) > 0u);

    knvr_watch_draw_boxes(NULL, FRAME_W, FRAME_H, boxes, 1u);
    knvr_watch_draw_boxes(frame, 0, 0, boxes, 1u);
    knvr_watch_draw_boxes(frame, FRAME_W, FRAME_H, NULL, 1u);
    free(frame);
    return true;
}

static bool
test_rejections(void)
{
    knvr_watch *watch = NULL;
    knvr_watch_options options;
    knvr_watch_stats stats;

    knvr_watch_options_init(&options);
    CHECK(!knvr_watch_start(NULL, "rtsp://host/1", &options));
    CHECK(!knvr_watch_start(&watch, NULL, &options));
    CHECK(!knvr_watch_start(&watch, "", &options));

    options.width = 0;
    CHECK(!knvr_watch_start(&watch, "rtsp://host/1", &options));
    CHECK(watch == NULL);

    CHECK(knvr_watch_width(NULL) == 0 && knvr_watch_height(NULL) == 0);
    CHECK(knvr_watch_error(NULL) == NULL);
    CHECK(!knvr_watch_step(NULL, NULL, NULL, 0u, NULL));

    (void)memset(&stats, 0xFF, sizeof(stats));
    knvr_watch_get_stats(NULL, &stats);
    CHECK(stats.frames == 0u);
    knvr_watch_get_stats(NULL, NULL);
    knvr_watch_stop(NULL);
    return true;
}

typedef bool (*test_function)(void);

typedef struct test_case {
    const char *name;
    test_function function;
} test_case;

int
main(void)
{
    static const test_case tests[] = {
        {"defaults are the measured ones",
         test_defaults_are_the_measured_ones},
        {"boxes draw their outline", test_boxes_draw_their_outline},
        {"edges and degenerate boxes", test_edges_and_degenerate_boxes},
        {"rejections", test_rejections}
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
