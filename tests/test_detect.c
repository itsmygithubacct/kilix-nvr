#include "knvr_detect.h"

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

#define W 64
#define H 48

static const char *const FAKE[] = {"python3", "tests/fake_detect.py", NULL};

static bool start(knvr_detector **detector, float min_score)
{
    knvr_detector_options options;

    knvr_detector_options_init(&options);
    options.argv = FAKE;
    options.width = W;
    options.height = H;
    options.min_score = min_score;
    options.timeout_seconds = 10;
    return knvr_detector_start(detector, &options);
}

/* The framing is the sizes themselves, so the thing worth proving is that
 * many frames in a row stay in step. */
static bool
test_the_pipe_stays_in_step(void)
{
    knvr_detector *detector = NULL;
    uint8_t *frame = calloc((size_t)W * H * 4u, 1u);
    knvr_detection_box boxes[8];
    size_t count = 0u;

    CHECK(frame != NULL);
    CHECK(start(&detector, 0.25f));
    for (int i = 0; i < 25; i++) {
        CHECK(knvr_detector_run(detector, frame, boxes, 8u, &count));
        CHECK(count == 1u);
        CHECK(boxes[0].class_id == 0);
        CHECK(boxes[0].score > 0.89f);
        /* 0.25..0.75 of a 64x48 frame, in pixels. */
        CHECK(boxes[0].x == 16 && boxes[0].y == 12);
        CHECK(boxes[0].w == 32 && boxes[0].h == 24);
    }
    knvr_detector_stop(detector);
    free(frame);
    return true;
}

/* The allowlist is the defence, not the threshold: a class nobody asked
 * for is dropped whatever it scored. */
static bool
test_the_allowlist_filters(void)
{
    knvr_detector *detector = NULL;
    uint8_t *frame = calloc((size_t)W * H * 4u, 1u);
    knvr_detection_box boxes[8];
    size_t count = 0u;

    CHECK(frame != NULL);
    CHECK(strcmp(knvr_detect_label(0), "person") == 0);
    CHECK(strcmp(knvr_detect_label(16), "dog") == 0);
    /* 61 is a toilet, which these models invented on still footage. */
    CHECK(knvr_detect_label(61) == NULL);
    CHECK(knvr_detect_label(-1) == NULL);
    CHECK(knvr_detect_label(999) == NULL);

    (void)setenv("FAKE_DETECT_CLASS", "61", 1);
    CHECK(start(&detector, 0.25f));
    CHECK(knvr_detector_run(detector, frame, boxes, 8u, &count));
    CHECK(count == 0u);   /* scored 0.9 and still dropped */
    knvr_detector_stop(detector);
    (void)unsetenv("FAKE_DETECT_CLASS");
    free(frame);
    return true;
}

static bool
test_the_threshold_applies(void)
{
    knvr_detector *detector = NULL;
    uint8_t *frame = calloc((size_t)W * H * 4u, 1u);
    knvr_detection_box boxes[8];
    size_t count = 0u;

    CHECK(frame != NULL);
    (void)setenv("FAKE_DETECT_SCORE", "0.10", 1);
    CHECK(start(&detector, 0.25f));
    CHECK(knvr_detector_run(detector, frame, boxes, 8u, &count));
    CHECK(count == 0u);
    knvr_detector_stop(detector);

    /* And the measured default keeps what 0.45 would have thrown away. */
    (void)setenv("FAKE_DETECT_SCORE", "0.30", 1);
    CHECK(start(&detector, 0.25f));
    CHECK(knvr_detector_run(detector, frame, boxes, 8u, &count));
    CHECK(count == 1u);
    knvr_detector_stop(detector);
    (void)unsetenv("FAKE_DETECT_SCORE");
    free(frame);
    return true;
}

/* A detector that will not start is a degradation, not a fault: the
 * caller keeps recording and keeps doing motion. */
static bool
test_a_dead_detector_is_survivable(void)
{
    static const char *const MISSING[] = {"kilix-nvr-no-such-detector", NULL};
    knvr_detector_options options;
    knvr_detector *detector = NULL;
    uint8_t *frame = calloc((size_t)W * H * 4u, 1u);
    knvr_detection_box boxes[4];
    size_t count = 0u;

    CHECK(frame != NULL);
    knvr_detector_options_init(&options);
    options.argv = MISSING;
    options.width = W;
    options.height = H;
    options.timeout_seconds = 2;
    /* The fork succeeds; the exec does not, which the first exchange
     * discovers. */
    if (knvr_detector_start(&detector, &options)) {
        CHECK(!knvr_detector_run(detector, frame, boxes, 4u, &count));
        CHECK(count == 0u);
        CHECK(knvr_detector_error(detector) != NULL);
        /* And it stays broken rather than retrying forever. */
        CHECK(!knvr_detector_run(detector, frame, boxes, 4u, &count));
        knvr_detector_stop(detector);
    }
    free(frame);
    return true;
}

static bool
test_defaults_and_rejections(void)
{
    knvr_detector_options options;
    knvr_detector *detector = NULL;

    knvr_detector_options_init(&options);
    /* Measured: 0.45 dropped real people in infrared. */
    CHECK(options.min_score > 0.24f && options.min_score < 0.26f);
    CHECK(KNVR_DETECT_BYTES == 480);

    CHECK(!knvr_detector_start(NULL, &options));
    options.width = 0;
    CHECK(!knvr_detector_start(&detector, &options));
    CHECK(!knvr_detector_run(NULL, NULL, NULL, 0u, NULL));
    CHECK(knvr_detector_error(NULL) == NULL);
    knvr_detector_stop(NULL);
    knvr_detector_options_init(NULL);
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
        {"the pipe stays in step", test_the_pipe_stays_in_step},
        {"the allowlist filters", test_the_allowlist_filters},
        {"the threshold applies", test_the_threshold_applies},
        {"a dead detector is survivable",
         test_a_dead_detector_is_survivable},
        {"defaults and rejections", test_defaults_and_rejections}
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
