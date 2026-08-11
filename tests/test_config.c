#include "knvr_config.h"
#include "knvr_paths.h"

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

static const char *STORE = "build/test-config.db";

static bool fresh_store(knvr_config **config)
{
    (void)remove(STORE);
    return knvr_config_open(config, STORE);
}

/*
 * The property the whole design leans on: a camera that has just been
 * added does nothing at all.  If this ever becomes false, typing a URL
 * starts filling a disk.
 */
static bool
test_a_new_camera_does_nothing(void)
{
    knvr_config *config = NULL;
    knvr_camera camera;

    CHECK(fresh_store(&config));
    CHECK(knvr_config_add(config, "hallway"));
    CHECK(knvr_config_get(config, "hallway", &camera));

    CHECK(camera.record == KNVR_RECORD_OFF);
    CHECK(camera.detect == KNVR_DETECT_OFF);
    CHECK(!camera.motion);
    CHECK(!camera.audio);
    CHECK(!camera.sound_events);
    CHECK(camera.retain_days == 0);
    CHECK(camera.mask[0] == '\0');
    CHECK(strcmp(camera.name, "hallway") == 0);

    knvr_config_close(config);
    return true;
}

/* Adding the same name twice is refused rather than silently overwriting:
 * a policy replaced because a name was typed twice is a recording that
 * stops with nobody noticing. */
static bool
test_names_and_duplicates(void)
{
    knvr_config *config = NULL;
    knvr_camera camera;

    CHECK(fresh_store(&config));
    CHECK(knvr_config_add(config, "yard"));
    CHECK(!knvr_config_add(config, "yard"));
    CHECK(knvr_config_error(config) != NULL);

    /* Names become path components for masks and media, so the character
     * set is restricted here rather than escaped at every use. */
    CHECK(!knvr_config_add(config, "../escape"));
    CHECK(!knvr_config_add(config, "with space"));
    CHECK(!knvr_config_add(config, "semi;colon"));
    CHECK(!knvr_config_add(config, ""));
    CHECK(!knvr_config_add(config, NULL));

    CHECK(!knvr_config_get(config, "nosuch", &camera));
    CHECK(knvr_config_remove(config, "yard"));
    CHECK(!knvr_config_remove(config, "yard"));

    knvr_config_close(config);
    return true;
}

static bool
test_settings_parse_and_refuse(void)
{
    knvr_camera camera;
    const char *reason = NULL;

    (void)memset(&camera, 0, sizeof(camera));
    (void)snprintf(camera.name, sizeof(camera.name), "%s", "cam");

    CHECK(knvr_camera_set(&camera, "record=continuous", &reason));
    CHECK(camera.record == KNVR_RECORD_CONTINUOUS);
    CHECK(knvr_camera_set(&camera, "record=stills", &reason));
    CHECK(camera.record == KNVR_RECORD_STILLS);
    CHECK(knvr_camera_set(&camera, "detect=on-view", &reason));
    CHECK(camera.detect == KNVR_DETECT_ON_VIEW);
    CHECK(knvr_camera_set(&camera, "motion=on", &reason) && camera.motion);
    CHECK(knvr_camera_set(&camera, "motion=false", &reason) &&
          !camera.motion);
    CHECK(knvr_camera_set(&camera, "audio=1", &reason) && camera.audio);
    CHECK(knvr_camera_set(&camera, "sound_events=yes", &reason) &&
          camera.sound_events);
    CHECK(knvr_camera_set(&camera, "retain_days=30", &reason) &&
          camera.retain_days == 30);
    CHECK(knvr_camera_set(&camera, "mask=masks/yard.mask.png", &reason));

    /* Every refusal says why, because the alternative is an operator
     * retyping a setting that was never going to be accepted. */
    CHECK(!knvr_camera_set(&camera, "record=sometimes", &reason));
    CHECK(reason != NULL);
    CHECK(!knvr_camera_set(&camera, "detect=maybe", &reason) &&
          reason != NULL);
    CHECK(!knvr_camera_set(&camera, "motion=perhaps", &reason) &&
          reason != NULL);
    CHECK(!knvr_camera_set(&camera, "nosuchkey=1", &reason) &&
          reason != NULL);
    CHECK(!knvr_camera_set(&camera, "novalue", &reason) && reason != NULL);
    CHECK(!knvr_camera_set(&camera, "=novalue", &reason) && reason != NULL);

    /* Out of range is refused, not clamped.  A retention quietly reduced
     * to something nobody asked for is a promise about how long footage
     * lasts that was never made. */
    CHECK(!knvr_camera_set(&camera, "retain_days=-1", &reason));
    CHECK(!knvr_camera_set(&camera, "retain_days=99999", &reason));
    CHECK(!knvr_camera_set(&camera, "retain_days=ten", &reason));
    CHECK(camera.retain_days == 30);
    return true;
}

/* Every spelling `set` accepts is one `cameras` prints, so the two cannot
 * describe the same state differently. */
static bool
test_names_round_trip_through_their_spellings(void)
{
    knvr_camera camera;
    const char *reason = NULL;

    (void)memset(&camera, 0, sizeof(camera));
    for (int mode = KNVR_RECORD_OFF; mode <= KNVR_RECORD_CONTINUOUS; mode++) {
        char assignment[64];

        (void)snprintf(assignment, sizeof(assignment), "record=%s",
                       knvr_record_mode_name((knvr_record_mode)mode));
        /*
         * `clips` is the one spelling that prints and does not set.
         *
         * It has a name because a store written before the check can hold
         * it and a camera has to be describable; it is refused because
         * nothing cuts clips yet, and accepting it meant a camera
         * reporting a mode it did not have.  The asymmetry is deliberate,
         * so it is asserted rather than tolerated.
         */
        if (mode == KNVR_RECORD_CLIPS) {
            reason = NULL;
            CHECK(!knvr_camera_set(&camera, assignment, &reason));
            CHECK(reason != NULL);
            CHECK(strstr(reason, "not implemented") != NULL);
            CHECK((int)camera.record != mode);
            continue;
        }
        CHECK(knvr_camera_set(&camera, assignment, &reason));
        CHECK((int)camera.record == mode);
    }
    for (int policy = KNVR_DETECT_OFF; policy <= KNVR_DETECT_ON_VIEW;
         policy++) {
        char assignment[64];

        (void)snprintf(assignment, sizeof(assignment), "detect=%s",
                       knvr_detect_policy_name((knvr_detect_policy)policy));
        CHECK(knvr_camera_set(&camera, assignment, &reason));
        CHECK((int)camera.detect == policy);
    }
    return true;
}

static bool
test_policy_survives_a_reopen(void)
{
    knvr_config *config = NULL;
    knvr_camera camera;
    const char *reason = NULL;

    CHECK(fresh_store(&config));
    CHECK(knvr_config_add(config, "drive"));
    CHECK(knvr_config_get(config, "drive", &camera));
    CHECK(knvr_camera_set(&camera, "record=continuous", &reason));
    CHECK(knvr_camera_set(&camera, "retain_days=7", &reason));
    CHECK(knvr_camera_set(&camera, "mask=masks/drive.mask.png", &reason));
    CHECK(knvr_config_put(config, &camera));
    knvr_config_close(config);

    CHECK(knvr_config_open(&config, STORE));
    (void)memset(&camera, 0, sizeof(camera));
    CHECK(knvr_config_get(config, "drive", &camera));
    CHECK(camera.record == KNVR_RECORD_CONTINUOUS);
    CHECK(camera.retain_days == 7);
    CHECK(strcmp(camera.mask, "masks/drive.mask.png") == 0);

    /* Writing a camera that does not exist is an error rather than an
     * insert: it means a name was mistyped, and inventing the row hides
     * that until somebody wonders why nothing records. */
    (void)snprintf(camera.name, sizeof(camera.name), "%s", "ghost");
    CHECK(!knvr_config_put(config, &camera));

    knvr_config_close(config);
    return true;
}

static bool
test_listing_reports_what_it_could_not_fit(void)
{
    knvr_config *config = NULL;
    knvr_camera cameras[2];
    size_t count = 0u;

    CHECK(fresh_store(&config));
    for (int i = 0; i < 5; i++) {
        char name[KNVR_NAME_MAX];

        (void)snprintf(name, sizeof(name), "cam%d", i);
        CHECK(knvr_config_add(config, name));
    }
    CHECK(knvr_config_list(config, cameras, 2u, &count));
    CHECK(count == 5u);   /* what exists, not what fitted */
    CHECK(strcmp(cameras[0].name, "cam0") == 0);
    CHECK(strcmp(cameras[1].name, "cam1") == 0);

    CHECK(knvr_config_list(config, NULL, 0u, &count));
    CHECK(count == 5u);

    knvr_config_close(config);
    return true;
}

/* on-view is honest about being blind, and only `cameras` says so. */
static bool
test_on_view_blindness(void)
{
    knvr_camera camera;

    (void)memset(&camera, 0, sizeof(camera));
    camera.detect = KNVR_DETECT_ON_VIEW;
    CHECK(knvr_camera_is_blind(&camera, false));
    CHECK(!knvr_camera_is_blind(&camera, true));

    camera.detect = KNVR_DETECT_ALWAYS;
    CHECK(!knvr_camera_is_blind(&camera, false));
    camera.detect = KNVR_DETECT_OFF;
    CHECK(!knvr_camera_is_blind(&camera, false));
    CHECK(!knvr_camera_is_blind(NULL, false));
    return true;
}

static bool
test_rejections(void)
{
    knvr_config *config = NULL;
    knvr_camera camera;
    size_t count = 0u;

    CHECK(!knvr_config_open(NULL, STORE));
    CHECK(!knvr_config_open(&config, "/definitely/not/here/store.db"));

    CHECK(fresh_store(&config));
    CHECK(!knvr_config_get(config, "x", NULL));
    CHECK(!knvr_config_put(config, NULL));
    CHECK(!knvr_config_list(NULL, &camera, 1u, &count));
    CHECK(!knvr_camera_set(NULL, "motion=on", NULL));
    CHECK(!knvr_camera_set(&camera, NULL, NULL));
    knvr_config_close(config);
    knvr_config_close(NULL);
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
        {"a new camera does nothing", test_a_new_camera_does_nothing},
        {"names and duplicates", test_names_and_duplicates},
        {"settings parse and refuse", test_settings_parse_and_refuse},
        {"names round trip through their spellings",
         test_names_round_trip_through_their_spellings},
        {"policy survives a reopen", test_policy_survives_a_reopen},
        {"listing reports what it could not fit",
         test_listing_reports_what_it_could_not_fit},
        {"on-view blindness", test_on_view_blindness},
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
    (void)remove(STORE);
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
