/*
 * kilix-nvr — a terminal-native network video recorder.
 *
 * What a camera does is its configuration, not an invocation: there is no
 * `record` command and no `detect` command.  `add` onboards one with every
 * capability off, `set` turns them on, `cameras` says what each is doing,
 * and `run` honours it.
 */

#include "knvr_config.h"
#include "knvr_detect.h"
#include "knvr_paths.h"
#include "knvr_store.h"
#include "knvr_watch.h"

#include "kilix_rtsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int usage(FILE *stream)
{
    (void)fprintf(
        stream,
        "usage: kilix-nvr <command> [options]\n"
        "\n"
        "  add <name>              onboard a camera; every capability off\n"
        "  set <name> <k=v>...     change what it does\n"
        "  cameras                 what exists and what each one is doing\n"
        "  remove <name>           forget a camera's policy\n"
        "  watch <name> [--seconds N] [--render OUT.ppm]\n"
        "                          decode one camera and record what moves\n"
        "  events [--since SECONDS] [--camera NAME] [--min-score S]\n"
        "                          what happened\n"
        "  prune [--dry-run] [--cap-mb N]\n"
        "                          apply retention: per-camera days, then\n"
        "                          a global size cap behind it\n"
        "  --selftest              check this build end to end\n"
        "\n"
        "Capabilities: record=off|stills|clips|continuous,\n"
        "detect=off|always|on-view, motion=on|off, audio=on|off,\n"
        "sound_events=on|off, retain_days=N, mask=PATH\n"
        "\n"
        "Cameras and their URLs come from kilix-rtsp's cameras.conf; this\n"
        "stores only what to do with them.  Data lives under\n"
        "~/.local/gpu_terminal/kilix-nvr, overridable with KILIX_NVR_HOME.\n");
    return 2;
}

/* Whether kilix-rtsp knows this name, so `add` can say when a policy would
 * refer to a camera that does not exist.  Advisory: the config file may
 * legitimately not be readable yet. */
static bool rtsp_knows(const char *name, bool *checked)
{
    krtsp_config *config = NULL;
    char directory[1024];
    char path[1088];
    char error[256];
    bool known = false;

    *checked = false;
    if (!krtsp_paths_dir("config", directory, sizeof(directory))) {
        return false;
    }
    if (snprintf(path, sizeof(path), "%s/cameras.conf", directory) < 0) {
        return false;
    }
    if (!krtsp_config_load(&config, path, error, sizeof(error))) {
        return false;
    }
    *checked = true;
    for (size_t i = 0u; i < krtsp_config_camera_count(config); i++) {
        const krtsp_camera *camera = krtsp_config_camera_at(config, i);

        if (camera != NULL && strcmp(camera->name, name) == 0) {
            known = true;
            break;
        }
    }
    krtsp_config_free(config);
    return known;
}

static int command_add(knvr_config *config, int argc, char **argv)
{
    bool checked = false;

    if (argc != 1) {
        return usage(stderr);
    }
    if (!knvr_config_add(config, argv[0])) {
        (void)fprintf(stderr, "kilix-nvr: %s\n",
                      knvr_config_error(config) != NULL
                          ? knvr_config_error(config)
                          : "cannot add that camera");
        return 1;
    }
    if (!rtsp_knows(argv[0], &checked) && checked) {
        /* Said, not refused: the policy is still valid and the URL can be
         * added afterwards.  Silence here would mean a camera that never
         * records and never explains why. */
        (void)fprintf(stderr,
                      "kilix-nvr: note: no camera named '%s' in "
                      "cameras.conf yet\n",
                      argv[0]);
    }
    (void)printf("%s added; every capability is off\n", argv[0]);
    return 0;
}

static int command_set(knvr_config *config, int argc, char **argv)
{
    knvr_camera camera;

    if (argc < 2) {
        return usage(stderr);
    }
    if (!knvr_config_get(config, argv[0], &camera)) {
        (void)fprintf(stderr, "kilix-nvr: no camera named '%s'\n", argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        const char *reason = NULL;

        if (!knvr_camera_set(&camera, argv[i], &reason)) {
            (void)fprintf(stderr, "kilix-nvr: %s: %s\n", argv[i],
                          reason != NULL ? reason : "rejected");
            return 1;
        }
    }
    if (!knvr_config_put(config, &camera)) {
        (void)fprintf(stderr, "kilix-nvr: %s\n",
                      knvr_config_error(config) != NULL
                          ? knvr_config_error(config)
                          : "cannot save");
        return 1;
    }
    (void)printf("%s updated\n", camera.name);
    return 0;
}

static int command_cameras(knvr_config *config)
{
    knvr_camera cameras[KNVR_CAMERA_MAX];
    size_t count = 0u;

    if (!knvr_config_list(config, cameras, KNVR_CAMERA_MAX, &count)) {
        (void)fprintf(stderr, "kilix-nvr: cannot list cameras\n");
        return 1;
    }
    if (count == 0u) {
        (void)printf("no cameras yet; add one with `kilix-nvr add <name>`\n");
        return 0;
    }
    (void)printf("%-16s %-11s %-8s %-7s %-6s %s\n", "CAMERA", "RECORD",
                 "DETECT", "MOTION", "AUDIO", "RETAIN");
    for (size_t i = 0u; i < count && i < KNVR_CAMERA_MAX; i++) {
        const knvr_camera *camera = &cameras[i];
        char retain[16];

        if (camera->retain_days > 0) {
            (void)snprintf(retain, sizeof(retain), "%dd",
                           camera->retain_days);
        } else {
            (void)snprintf(retain, sizeof(retain), "-");
        }
        (void)printf("%-16s %-11s %-8s %-7s %-6s %s", camera->name,
                     knvr_record_mode_name(camera->record),
                     knvr_detect_policy_name(camera->detect),
                     camera->motion ? "on" : "off",
                     camera->audio ? "on" : "off", retain);
        /* The one place on-view blindness is reported.  A warning on every
         * frame would train people to ignore it. */
        if (knvr_camera_is_blind(camera, false)) {
            (void)printf("   (not watching: detects only while viewed)");
        }
        (void)printf("\n");
    }
    if (count > KNVR_CAMERA_MAX) {
        (void)printf("... and %zu more\n", count - KNVR_CAMERA_MAX);
    }
    return 0;
}

#define QUIET_SECONDS 5

#define TEST(name, condition)                                                 \
    do {                                                                      \
        const bool passed = (condition);                                      \
        (void)printf("%s %s\n", passed ? "ok" : "not ok", (name));            \
        if (!passed) {                                                        \
            return 1;                                                         \
        }                                                                     \
    } while (false)

/*
 * A smoke test of this binary against a throwaway store, so it can be run
 * on a machine that has the program installed but not this source tree.
 */
static int selftest(void)
{
    knvr_config *config = NULL;
    knvr_camera camera;
    const char *reason = NULL;
    char path[KNVR_PATH_MAX];

    TEST("a state path resolves", knvr_paths_state_file(path, sizeof(path),
                                                        "selftest.db"));
    (void)remove(path);
    TEST("the policy store opens", knvr_config_open(&config, path));
    TEST("a camera can be added", knvr_config_add(config, "selftest-cam"));
    TEST("adding it twice is refused",
         !knvr_config_add(config, "selftest-cam"));
    TEST("a name with a slash is refused",
         !knvr_config_add(config, "../escape"));

    TEST("it reads back", knvr_config_get(config, "selftest-cam", &camera));
    TEST("with everything off",
         camera.record == KNVR_RECORD_OFF &&
             camera.detect == KNVR_DETECT_OFF && !camera.motion &&
             !camera.audio && !camera.sound_events &&
             camera.retain_days == 0);

    TEST("capabilities parse",
         knvr_camera_set(&camera, "record=continuous", &reason) &&
             knvr_camera_set(&camera, "detect=on-view", &reason) &&
             knvr_camera_set(&camera, "motion=on", &reason) &&
             knvr_camera_set(&camera, "retain_days=14", &reason));
    TEST("nonsense is refused",
         !knvr_camera_set(&camera, "record=sometimes", &reason) &&
             !knvr_camera_set(&camera, "retain_days=-1", &reason) &&
             !knvr_camera_set(&camera, "nosuchkey=1", &reason) &&
             !knvr_camera_set(&camera, "novalue", &reason));
    TEST("and says why", reason != NULL);

    TEST("it saves", knvr_config_put(config, &camera));
    (void)memset(&camera, 0, sizeof(camera));
    TEST("and survives a reopen", knvr_config_get(config, "selftest-cam",
                                                  &camera));
    TEST("with what was set",
         camera.record == KNVR_RECORD_CONTINUOUS &&
             camera.detect == KNVR_DETECT_ON_VIEW && camera.motion &&
             camera.retain_days == 14);
    TEST("on-view reports blindness when nobody is watching",
         knvr_camera_is_blind(&camera, false) &&
             !knvr_camera_is_blind(&camera, true));

    TEST("it can be removed", knvr_config_remove(config, "selftest-cam"));
    TEST("and is gone", !knvr_config_get(config, "selftest-cam", &camera));

    knvr_config_close(config);
    (void)remove(path);
    (void)printf("selftest passed\n");
    return 0;
}

static int command_watch(knvr_config *config, int argc, char **argv);
static int command_events(int argc, char **argv);
static int command_prune(knvr_config *config, int argc, char **argv);

int main(int argc, char **argv)
{
    knvr_config *config = NULL;
    const char *command;
    int status;

    if (argc < 2) {
        return usage(stderr);
    }
    command = argv[1];
    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0 ||
        strcmp(command, "help") == 0) {
        (void)usage(stdout);
        return 0;
    }
    if (strcmp(command, "--selftest") == 0) {
        return selftest();
    }
    if (!knvr_config_open(&config, NULL)) {
        (void)fprintf(stderr, "kilix-nvr: cannot open the policy store\n");
        return 1;
    }
    if (strcmp(command, "add") == 0) {
        status = command_add(config, argc - 2, argv + 2);
    } else if (strcmp(command, "set") == 0) {
        status = command_set(config, argc - 2, argv + 2);
    } else if (strcmp(command, "cameras") == 0) {
        status = command_cameras(config);
    } else if (strcmp(command, "watch") == 0) {
        status = command_watch(config, argc - 2, argv + 2);
    } else if (strcmp(command, "events") == 0) {
        status = command_events(argc - 2, argv + 2);
    } else if (strcmp(command, "prune") == 0) {
        status = command_prune(config, argc - 2, argv + 2);
    } else if (strcmp(command, "remove") == 0) {
        status = argc == 3 && knvr_config_remove(config, argv[2]) ? 0 : 1;
        if (status != 0) {
            (void)fprintf(stderr, "kilix-nvr: no such camera\n");
        }
    } else {
        status = usage(stderr);
    }
    knvr_config_close(config);
    return status;
}

/* ------------------------------ watch ----------------------------------- */

/*
 * Resolve a camera's stream URL from kilix-rtsp's config.
 *
 * Copied into the caller's buffer and never logged: krtsp_url_redact()
 * exists because an RTSP URL carries credentials, and the only way to
 * honour that is for this program never to print one.
 */
static bool resolve_url(const char *name, bool prefer_sub, char *out,
                        size_t size)
{
    krtsp_config *config = NULL;
    char directory[1024];
    char path[1088];
    char error[256];
    bool found = false;

    if (!krtsp_paths_dir("config", directory, sizeof(directory))) {
        return false;
    }
    if (snprintf(path, sizeof(path), "%s/cameras.conf", directory) < 0) {
        return false;
    }
    if (!krtsp_config_load(&config, path, error, sizeof(error))) {
        (void)fprintf(stderr, "kilix-nvr: %s\n", error);
        return false;
    }
    for (size_t i = 0u; i < krtsp_config_camera_count(config); i++) {
        const krtsp_camera *camera = krtsp_config_camera_at(config, i);

        if (camera == NULL || strcmp(camera->name, name) != 0) {
            continue;
        }
        {
            const char *url = prefer_sub && camera->url_sub[0] != '\0'
                                  ? camera->url_sub : camera->url_main;

            if (url[0] != '\0' && strlen(url) < size) {
                (void)snprintf(out, size, "%s", url);
                found = true;
            }
        }
        break;
    }
    krtsp_config_free(config);
    return found;
}

static int write_ppm(const char *path, const uint8_t *bgra, int width,
                     int height)
{
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        return 1;
    }
    (void)fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        /* BGRA in, RGB out. */
        (void)fputc(bgra[i * 4 + 2], file);
        (void)fputc(bgra[i * 4 + 1], file);
        (void)fputc(bgra[i * 4 + 0], file);
    }
    return fclose(file) == 0 ? 0 : 1;
}

/*
 * One frame of the event, on disk and indexed.
 *
 * Written once per event rather than per detection: twenty stills of the
 * same person crossing the same yard is a directory nobody reads.
 */
static void save_still(knvr_store *store, int64_t event_id,
                       const char *camera, const uint8_t *frame, int width,
                       int height)
{
    char directory[KNVR_PATH_MAX];
    char path[KNVR_PATH_MAX];
    static int64_t last_event;

    if (event_id == 0 || event_id == last_event) {
        return;
    }
    if (!knvr_paths_subdir(directory, sizeof(directory), "media")) {
        return;
    }
    if (snprintf(path, sizeof(path), "%s/%s-%lld.ppm", directory, camera,
                 (long long)event_id) < 0) {
        return;
    }
    if (write_ppm(path, frame, width, height) == 0) {
        (void)knvr_store_add_media(store, event_id, "still", path);
        (void)printf("    still %s\n", path);
        last_event = event_id;
    }
}

static int command_watch(knvr_config *config, int argc, char **argv)
{
    knvr_watch_options options;
    knvr_store *store = NULL;
    knvr_detector *detector = NULL;
    knvr_watch *watch = NULL;
    int64_t event_id = 0;
    time_t last_motion = 0;
    knvr_watch_stats stats;
    knvr_camera camera;
    knvr_box boxes[KNVR_MOTION_BOX_MAX];
    char url[KRTSP_URL_MAX];
    char mask_path[KNVR_PATH_MAX];
    char log_path[KNVR_PATH_MAX];
    const char *render = NULL;
    const char *name;
    int seconds = 10;
    int rendered = 0;
    time_t deadline;

    if (argc < 1) {
        return usage(stderr);
    }
    name = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--render") == 0 && i + 1 < argc) {
            render = argv[++i];
        } else {
            return usage(stderr);
        }
    }
    if (seconds <= 0 || seconds > 86400) {
        (void)fprintf(stderr, "kilix-nvr: --seconds is 1 to 86400\n");
        return 1;
    }
    if (!knvr_config_get(config, name, &camera)) {
        (void)fprintf(stderr,
                      "kilix-nvr: no policy for '%s'; add it first\n", name);
        return 1;
    }
    if (!resolve_url(name, true, url, sizeof(url))) {
        (void)fprintf(stderr,
                      "kilix-nvr: no stream URL for '%s' in cameras.conf\n",
                      name);
        return 1;
    }

    knvr_watch_options_init(&options);
    /* ffmpeg's stderr goes to a file, never the terminal: one warning in
     * the alternate screen corrupts the display, and a flaky camera
     * produces plenty. */
    if (knvr_paths_state_file(log_path, sizeof(log_path), "ffmpeg.log")) {
        options.log_path = log_path;
    }
    if (camera.record == KNVR_RECORD_CONTINUOUS) {
        static char record_dir[KNVR_PATH_MAX];
        static char record_url[KRTSP_URL_MAX];
        char segments[KNVR_PATH_MAX];

        /* The main stream for the archive, the substream for motion.
         * Full-quality footage without decoding it. */
        if (resolve_url(name, false, record_url, sizeof(record_url))) {
            options.record_url = record_url;
        }
        if (knvr_paths_subdir(segments, sizeof(segments), "segments") &&
            snprintf(record_dir, sizeof(record_dir), "%s/%s", segments,
                     name) > 0) {
            (void)mkdir(record_dir, 0700);
            options.record_dir = record_dir;
            /* Matroska by default, measured: pcm_alaw survives -c copy
             * untouched, which mp4 cannot promise. */
            options.segment_seconds = 60;
            options.record_audio = camera.audio;
        }
    }
    if (camera.mask[0] != '\0') {
        char masks[KNVR_PATH_MAX];

        if (knvr_paths_subdir(masks, sizeof(masks), "masks") &&
            snprintf(mask_path, sizeof(mask_path), "%s/%s", masks,
                     camera.mask) > 0) {
            options.mask_path = mask_path;
        }
    }
    if (!knvr_watch_start(&watch, url, &options)) {
        (void)fprintf(stderr, "kilix-nvr: %s: %s\n", name,
                      knvr_watch_error(watch) != NULL
                          ? knvr_watch_error(watch)
                          : "could not start the camera");
        knvr_watch_stop(watch);
        return 1;
    }
    (void)printf("watching %s at %dx%d for %ds%s\n", name,
                 knvr_watch_width(watch), knvr_watch_height(watch), seconds,
                 options.mask_path != NULL ? " (masked)" : "");

    /* `always` runs it.  `on-view` is blind with no viewer attached, and
     * `cameras` is the one place that says so. */
    if (camera.detect == KNVR_DETECT_ALWAYS) {
        knvr_detector_options detector_options;

        knvr_detector_options_init(&detector_options);
        detector_options.width = knvr_watch_width(watch);
        detector_options.height = knvr_watch_height(watch);
        if (!knvr_detector_start(&detector, &detector_options)) {
            /* A degradation, not a fault: motion-only still records. */
            (void)fprintf(stderr,
                          "kilix-nvr: %s: no detector; motion only\n", name);
        }
    }
    if (!knvr_store_open(&store, NULL)) {
        (void)fprintf(stderr, "kilix-nvr: cannot open the event store\n");
        knvr_watch_stop(watch);
        return 1;
    }
    /* Anything this camera left open belongs to a previous run.  An event
     * with no end time never appears in a query bounded by when it
     * finished, so a crash must not leave one behind. */
    (void)knvr_store_close_stale(store, name, (int64_t)time(NULL));

    deadline = time(NULL) + seconds;
    while (time(NULL) < deadline) {
        const uint8_t *frame = NULL;
        size_t count = 0u;

        if (!knvr_watch_step(watch, &frame, boxes, KNVR_MOTION_BOX_MAX,
                             &count)) {
            struct timespec pause = {0, 100 * 1000 * 1000};

            (void)nanosleep(&pause, NULL);
            continue;
        }
        if (count > 0u) {
            const int64_t at = (int64_t)time(NULL);

            if (event_id == 0) {
                if (knvr_store_event_open(store, name, KNVR_TRIGGER_MOTION,
                                          at, &event_id)) {
                    (void)printf("  event %lld opened\n",
                                 (long long)event_id);
                }
            } else {
                (void)knvr_store_event_touch(store, event_id);
            }
            last_motion = (time_t)at;
            (void)printf("  motion: %zu region%s\n", count,
                         count == 1u ? "" : "s");

            /* Gated on motion, which is the whole CPU story: inference is
             * 7-36 ms, differencing a downscaled frame is arithmetic. */
            if (detector != NULL) {
                knvr_detection_box found[KNVR_DETECT_ROWS];
                size_t detections = 0u;

                if (knvr_detector_run(detector, frame, found,
                                      KNVR_DETECT_ROWS, &detections)) {
                    for (size_t d = 0u; d < detections; d++) {
                        knvr_detection record;

                        (void)memset(&record, 0, sizeof(record));
                        record.event = event_id;
                        record.at = at;
                        (void)snprintf(record.label, sizeof(record.label),
                                       "%s",
                                       knvr_detect_label(found[d].class_id));
                        record.score = (double)found[d].score;
                        record.x = found[d].x;
                        record.y = found[d].y;
                        record.w = found[d].w;
                        record.h = found[d].h;
                        (void)knvr_store_add_detection(store, &record);
                        (void)printf("    %s %.2f\n", record.label,
                                     record.score);
                    }
                    if (detections > 0u && camera.record != KNVR_RECORD_OFF) {
                        save_still(store, event_id, name, frame,
                                   knvr_watch_width(watch),
                                   knvr_watch_height(watch));
                    }
                } else if (knvr_detector_error(detector) != NULL) {
                    (void)fprintf(stderr, "kilix-nvr: %s: %s\n", name,
                                  knvr_detector_error(detector));
                    knvr_detector_stop(detector);
                    detector = NULL;
                }
            }
            /* The first frame with motion is the one worth looking at,
             * so that is the one written. */
            if (render != NULL && rendered == 0) {
                uint8_t *copy = malloc((size_t)knvr_watch_width(watch) *
                                       (size_t)knvr_watch_height(watch) * 4u);

                if (copy != NULL) {
                    (void)memcpy(copy, frame,
                                 (size_t)knvr_watch_width(watch) *
                                     (size_t)knvr_watch_height(watch) * 4u);
                    knvr_watch_draw_boxes(copy, knvr_watch_width(watch),
                                          knvr_watch_height(watch), boxes,
                                          count);
                    if (write_ppm(render, copy, knvr_watch_width(watch),
                                  knvr_watch_height(watch)) == 0) {
                        (void)printf("  wrote %s\n", render);
                        rendered = 1;
                    }
                    free(copy);
                }
            }
        } else if (event_id != 0 &&
                   time(NULL) - last_motion >= QUIET_SECONDS) {
            /* Quiet for long enough to call it over.  Closing on the very
             * next still frame would split one person walking past into a
             * dozen events. */
            (void)knvr_store_event_close(store, event_id,
                                         (int64_t)time(NULL));
            (void)printf("  event %lld closed\n", (long long)event_id);
            event_id = 0;
        }
        {
            struct timespec pause = {0, 30 * 1000 * 1000};

            (void)nanosleep(&pause, NULL);
        }
    }
    if (event_id != 0) {
        (void)knvr_store_event_close(store, event_id, (int64_t)time(NULL));
    }
    knvr_watch_get_stats(watch, &stats);
    (void)printf("%llu frames, %llu with motion, %llu boxes, newest %dms old\n",
                 (unsigned long long)stats.frames,
                 (unsigned long long)stats.motion_frames,
                 (unsigned long long)stats.boxes, stats.last_age_ms);
    knvr_watch_stop(watch);
    knvr_detector_stop(detector);
    knvr_store_close(store);
    return 0;
}

/* ------------------------- events and retention -------------------------- */

static void print_when(int64_t at, char *out, size_t size)
{
    /* Stored UTC, shown local.  The conversion happens here, once: the
     * archive this was measured against is UTC while the machine is
     * UTC-7, and a "night" sample taken by local hour was an afternoon
     * one. */
    const time_t when = (time_t)at;
    struct tm parts;

    if (localtime_r(&when, &parts) == NULL) {
        (void)snprintf(out, size, "?");
        return;
    }
    (void)strftime(out, size, "%Y-%m-%d %H:%M:%S", &parts);
}

static int command_events(int argc, char **argv)
{
    knvr_store *store = NULL;
    knvr_query query;
    knvr_event events[256];
    size_t count = 0u;

    knvr_query_init(&query);
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--since") == 0 && i + 1 < argc) {
            query.since = (int64_t)time(NULL) - atoll(argv[++i]);
        } else if (strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            (void)snprintf(query.camera, sizeof(query.camera), "%s",
                           argv[++i]);
        } else if (strcmp(argv[i], "--min-score") == 0 && i + 1 < argc) {
            query.min_score = atof(argv[++i]);
        } else {
            return usage(stderr);
        }
    }
    if (!knvr_store_open(&store, NULL)) {
        (void)fprintf(stderr, "kilix-nvr: cannot open the event store\n");
        return 1;
    }
    if (!knvr_store_events(store, &query, events, 256u, &count)) {
        (void)fprintf(stderr, "kilix-nvr: cannot query events\n");
        knvr_store_close(store);
        return 1;
    }
    if (count == 0u) {
        (void)printf("nothing happened\n");
        knvr_store_close(store);
        return 0;
    }
    (void)printf("%-6s %-14s %-20s %-8s %-7s %s\n", "ID", "CAMERA", "STARTED",
                 "LASTED", "FRAMES", "BEST");
    for (size_t i = 0u; i < count && i < 256u; i++) {
        const knvr_event *event = &events[i];
        char started[32];
        char lasted[24];
        char best[48];

        print_when(event->started, started, sizeof(started));
        if (event->ended > 0) {
            (void)snprintf(lasted, sizeof(lasted), "%llds",
                           (long long)(event->ended - event->started));
        } else {
            (void)snprintf(lasted, sizeof(lasted), "open");
        }
        if (event->best_score > 0.0) {
            (void)snprintf(best, sizeof(best), "%s %.2f", event->best_label,
                           event->best_score);
        } else {
            (void)snprintf(best, sizeof(best), "-");
        }
        (void)printf("%-6lld %-14s %-20s %-8s %-7lld %s\n",
                     (long long)event->id, event->camera, started, lasted,
                     (long long)event->motion_frames, best);
    }
    if (count > 256u) {
        (void)printf("... and %zu more\n", count - 256u);
    }
    knvr_store_close(store);
    return 0;
}

static int command_prune(knvr_config *config, int argc, char **argv)
{
    knvr_store *store = NULL;
    knvr_camera cameras[KNVR_CAMERA_MAX];
    knvr_prune_result result;
    size_t count = 0u;
    size_t events_removed = 0u;
    uint64_t bytes_freed = 0u;
    uint64_t cap_bytes = 0u;
    bool dry_run = false;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(argv[i], "--cap-mb") == 0 && i + 1 < argc) {
            cap_bytes = (uint64_t)atoll(argv[++i]) * 1024u * 1024u;
        } else {
            return usage(stderr);
        }
    }
    if (!knvr_config_list(config, cameras, KNVR_CAMERA_MAX, &count)) {
        (void)fprintf(stderr, "kilix-nvr: cannot list cameras\n");
        return 1;
    }
    if (!knvr_store_open(&store, NULL)) {
        (void)fprintf(stderr, "kilix-nvr: cannot open the event store\n");
        return 1;
    }
    /* Per-camera age first, the global cap behind it.  A size cap alone
     * silently shortens the retention of whichever camera is busiest. */
    for (size_t i = 0u; i < count && i < KNVR_CAMERA_MAX; i++) {
        knvr_retention rule;

        if (cameras[i].retain_days <= 0) {
            continue;
        }
        (void)memset(&rule, 0, sizeof(rule));
        rule.days = cameras[i].retain_days;
        (void)snprintf(rule.camera, sizeof(rule.camera), "%.*s",
                       (int)sizeof(rule.camera) - 1, cameras[i].name);
        if (knvr_store_prune_age(store, &rule, (int64_t)time(NULL), dry_run,
                                 &result)) {
            events_removed += result.events_removed;
            bytes_freed += result.bytes_freed;
        }
    }
    if (cap_bytes > 0u &&
        knvr_store_prune_size(store, cap_bytes, dry_run, &result)) {
        events_removed += result.events_removed;
        bytes_freed += result.bytes_freed;
    }
    (void)printf("%s %zu event%s, %.1f MB\n",
                 dry_run ? "would remove" : "removed", events_removed,
                 events_removed == 1u ? "" : "s",
                 (double)bytes_freed / (1024.0 * 1024.0));
    knvr_store_close(store);
    return 0;
}
