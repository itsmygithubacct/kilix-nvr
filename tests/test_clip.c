/*
 * Segment retention, which is the part that deletes footage.
 *
 * Worth its own test for one reason: until now nothing pruned segments at
 * all.  They are written by a separate ffmpeg without telling the store,
 * so no media row referred to them and no rule reached them - a camera on
 * `continuous` filled the disk while `prune` reported success.  A rule
 * that deletes video needs to be shown deleting the right video and, more
 * importantly, not the wrong one.
 *
 * The cut itself is not tested here: it is ffmpeg copying a stream, and a
 * test of it would be a test of ffmpeg.
 */

#include "knvr_clip.h"
#include "knvr_paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

#define HOUR 3600
#define DAY (24 * HOUR)

static const int64_t NOW = 1760000000;
static const char *CONFIG = "build/test-clip-cameras.db";

/* A segment on disk, with the mtime the sweep reads as its age. */
static bool segment(const char *camera, const char *name, int64_t at)
{
    char segments[KNVR_PATH_MAX];
    char directory[KNVR_PATH_MAX];
    char path[KNVR_PATH_MAX];
    struct utimbuf when;
    FILE *file;

    if (!knvr_paths_subdir(segments, sizeof(segments), "segments")) {
        return false;
    }
    if (snprintf(directory, sizeof(directory), "%s/%s", segments, camera) < 0) {
        return false;
    }
    (void)mkdir(directory, 0700);
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) < 0) {
        return false;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    (void)fputs("xxxxxxxxxx", file);
    if (fclose(file) != 0) {
        return false;
    }
    when.actime = (time_t)at;
    when.modtime = (time_t)at;
    return utime(path, &when) == 0;
}

static bool exists(const char *camera, const char *name)
{
    char segments[KNVR_PATH_MAX];
    char path[KNVR_PATH_MAX];
    struct stat info;

    if (!knvr_paths_subdir(segments, sizeof(segments), "segments")) {
        return false;
    }
    if (snprintf(path, sizeof(path), "%s/%s/%s", segments, camera, name) < 0) {
        return false;
    }
    return stat(path, &info) == 0;
}

static bool camera(knvr_config *config, const char *name,
                   knvr_record_mode record, int retain_days)
{
    knvr_camera policy;

    if (!knvr_config_add(config, name) ||
        !knvr_config_get(config, name, &policy)) {
        return false;
    }
    policy.record = record;
    policy.retain_days = retain_days;
    return knvr_config_put(config, &policy);
}

/*
 * A `clips` camera keeps segments for hours, not days: the clips are its
 * archive, and holding both is paying for continuous twice.
 */
static bool test_clips_keeps_segments_only_long_enough_to_cut(void)
{
    knvr_config *config = NULL;
    knvr_prune_result result;

    (void)remove(CONFIG);
    CHECK(knvr_config_open(&config, CONFIG));
    CHECK(camera(config, "clipcam", KNVR_RECORD_CLIPS, 30));

    CHECK(segment("clipcam", "now.mkv", NOW - 60));
    CHECK(segment("clipcam", "recent.mkv", NOW - HOUR));
    CHECK(segment("clipcam", "old.mkv", NOW - 6 * HOUR));

    (void)memset(&result, 0, sizeof(result));
    CHECK(knvr_clip_prune_segments(config, NOW, false, &result));
    CHECK(exists("clipcam", "now.mkv"));
    /* Inside the window, because a cut that failed gets another chance. */
    CHECK(exists("clipcam", "recent.mkv"));
    CHECK(!exists("clipcam", "old.mkv"));
    CHECK(result.media_removed == 1u);
    CHECK(result.bytes_freed == 10u);

    /* retain_days is 30 and must NOT hold the segments: it is the clips'
     * retention, and confusing the two is how this mode becomes
     * continuous with extra steps. */
    knvr_config_close(config);
    return true;
}

/* A `continuous` camera's segments ARE the archive, so its days apply. */
static bool test_continuous_keeps_segments_for_its_days(void)
{
    knvr_config *config = NULL;
    knvr_prune_result result;

    (void)remove(CONFIG);
    CHECK(knvr_config_open(&config, CONFIG));
    CHECK(camera(config, "allcam", KNVR_RECORD_CONTINUOUS, 7));

    CHECK(segment("allcam", "yesterday.mkv", NOW - DAY));
    CHECK(segment("allcam", "lastweek.mkv", NOW - 9 * DAY));

    (void)memset(&result, 0, sizeof(result));
    CHECK(knvr_clip_prune_segments(config, NOW, false, &result));
    CHECK(exists("allcam", "yesterday.mkv"));
    CHECK(!exists("allcam", "lastweek.mkv"));
    CHECK(result.media_removed == 1u);
    knvr_config_close(config);
    return true;
}

/*
 * Two ways to be left alone, and both matter.
 *
 * A camera with no age rule is one whose retention nobody set, and
 * deleting its footage on a default it never chose is the one mistake a
 * recorder must not make.  A camera not recording video has segments only
 * from a mode it used to be in, and removing them because a setting
 * changed is not this function's decision either.
 */
static bool test_what_is_left_alone(void)
{
    knvr_config *config = NULL;
    knvr_prune_result result;

    (void)remove(CONFIG);
    CHECK(knvr_config_open(&config, CONFIG));
    CHECK(camera(config, "nodays", KNVR_RECORD_CONTINUOUS, 0));
    CHECK(camera(config, "stillscam", KNVR_RECORD_STILLS, 1));

    CHECK(segment("nodays", "ancient.mkv", NOW - 400 * DAY));
    CHECK(segment("stillscam", "leftover.mkv", NOW - 400 * DAY));

    (void)memset(&result, 0, sizeof(result));
    CHECK(knvr_clip_prune_segments(config, NOW, false, &result));
    CHECK(exists("nodays", "ancient.mkv"));
    CHECK(exists("stillscam", "leftover.mkv"));
    CHECK(result.media_removed == 0u);
    knvr_config_close(config);
    return true;
}

/* The first question about a delete is always "what exactly". */
static bool test_a_dry_run_removes_nothing(void)
{
    knvr_config *config = NULL;
    knvr_prune_result result;

    (void)remove(CONFIG);
    CHECK(knvr_config_open(&config, CONFIG));
    CHECK(camera(config, "drycam", KNVR_RECORD_CONTINUOUS, 1));
    CHECK(segment("drycam", "old.mkv", NOW - 30 * DAY));

    (void)memset(&result, 0, sizeof(result));
    CHECK(knvr_clip_prune_segments(config, NOW, true, &result));
    CHECK(exists("drycam", "old.mkv"));
    CHECK(result.media_removed == 1u);
    CHECK(result.bytes_freed == 10u);
    knvr_config_close(config);
    return true;
}

static bool test_rejections(void)
{
    CHECK(!knvr_clip_prune_segments(NULL, NOW, false, NULL));
    CHECK(!knvr_clip_segment_covering(NULL, NOW, NULL, 0u));
    CHECK(!knvr_clip_cut(NULL, NULL, NULL, 0u));
    CHECK(knvr_clip_start(NULL, NULL, 0u) < 0);
    /* Collecting a child that was never started is done, not pending. */
    CHECK(knvr_clip_finish(NULL, -1, 0, NULL, NULL));
    return true;
}

int main(void)
{
    /*
     * Its own data root, set before anything asks where things live.
     *
     * This test deletes video files.  Run against the real directory it
     * would sweep a camera that happened to share a name with one of
     * these, and a retention test that can delete a person's footage is
     * not a test anybody should have to be careful around.
     */
    char root[KNVR_PATH_MAX];
    char cwd[KNVR_PATH_MAX];

    /* Absolute, because that is the only form knvr_paths_home() honours -
     * a relative one is silently ignored and everything below lands in
     * the real directory, which is how this test first ran. */
    if (getcwd(cwd, sizeof(cwd)) == NULL ||
        snprintf(root, sizeof(root), "%s/build/test-clip-home", cwd) < 0 ||
        setenv("KILIX_NVR_HOME", root, 1) != 0) {
        (void)fprintf(stderr, "cannot isolate the data root\n");
        return 1;
    }
    const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        {"clips keeps segments only long enough to cut",
         test_clips_keeps_segments_only_long_enough_to_cut},
        {"continuous keeps segments for its days",
         test_continuous_keeps_segments_for_its_days},
        {"what is left alone", test_what_is_left_alone},
        {"a dry run removes nothing", test_a_dry_run_removes_nothing},
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
