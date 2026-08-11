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
#include "knvr_review.h"
#include "knvr_view.h"
#include "kilix_sound_detect.h"
#include "knvr_track.h"
#include "knvr_zone.h"

#include "soft_raster.h"
#include "knvr_store.h"
#include "knvr_watch.h"

#include "kilix_rtsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
        "         [--zone NAME]    what happened\n"
        "  objects <event>         the things it was, not the frames\n"
        "  zones <name>            a camera's zones and what they do\n"
        "  zone add <name> <zone> [inertia=N] [preclusive=yes] [loiter=S]\n"
        "  zone remove <name> <zone>\n"
        "  zone paint <name>       grab a frame and paint its zones\n"
        "  view [name] [--replay]  watch the cameras: the picture, what the\n"
        "                          models make of it, and motion and sound\n"
        "                          drawn as waveforms under each one.  r on\n"
        "                          an event replays it; l returns to live\n"
        "  review                  browse events with the frame that caused\n"
        "                          them; up/down to select, q to quit\n"
        "  play <event>            print the footage covering an event\n"
        "  clip <event>            cut it out of the segments, no re-encode\n"
        "  reanalyze <event>       run the detector over its still again\n"
        "  prune [--dry-run] [--cap-mb N]\n"
        "                          apply retention: per-camera days, then\n"
        "                          a global size cap behind it\n"
        "  --selftest              check this build end to end\n"
        "\n"
        "Capabilities: record=off|stills|clips|continuous,\n"
        "detect=off|always|on-view, motion=on|off, audio=on|off,\n"
        "sound_events=on|off, retain_days=N, mask=PATH, zones=PATH\n"
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

    /* Tracking: the property everything downstream rests on is that the
     * same thing keeps one id. */
    {
        knvr_tracker *tracker = NULL;
        knvr_detection_box seen;
        int64_t first;

        (void)memset(&seen, 0, sizeof(seen));
        seen.class_id = 0;
        seen.score = 0.9f;
        seen.x = 100; seen.y = 100; seen.w = 40; seen.h = 80;
        TEST("a tracker starts", knvr_tracker_create(&tracker, NULL));
        TEST("it takes a detection",
             knvr_tracker_update(tracker, &seen, 1u, 1000));
        first = knvr_tracker_assigned(tracker, 0u);
        seen.x = 112;
        TEST("and the same object keeps its id",
             knvr_tracker_update(tracker, &seen, 1u, 1300) &&
                 knvr_tracker_assigned(tracker, 0u) == first);
        seen.x = 460;
        seen.y = 300;
        TEST("while something elsewhere is a new one",
             knvr_tracker_update(tracker, &seen, 1u, 1600) &&
                 knvr_tracker_assigned(tracker, 0u) != first);
        knvr_tracker_free(tracker);
    }

    /* Zones: define, read back, and remove. */
    {
        knvr_zones *zones = NULL;
        const char *why = NULL;
        uint8_t region = 0u;
        char map[KNVR_PATH_MAX];

        TEST("a zone map path resolves",
             knvr_paths_state_file(map, sizeof(map), "selftest-zones.png"));
        (void)remove(map);
        TEST("a zone can be defined",
             knvr_zones_define(map, "selftest-zone", 640, 360, 2, true, 15,
                               &region, &why));
        TEST("with the first free region", region == 1u);
        TEST("the same name twice is refused",
             !knvr_zones_define(map, "selftest-zone", 0, 0, 1, false, 0,
                                &region, &why) && why != NULL);
        TEST("it reads back", knvr_zones_load(&zones, map, 640, 360));
        TEST("with its policy",
             knvr_zones_find(zones, "selftest-zone") != NULL &&
                 knvr_zones_find(zones, "selftest-zone")->inertia == 2 &&
                 knvr_zones_find(zones, "selftest-zone")->preclusive &&
                 knvr_zone_is_preclusive(zones, 1u));
        knvr_zones_free(zones);
        TEST("and can be removed",
             knvr_zones_undefine(map, "selftest-zone", &why));
        TEST("only once",
             !knvr_zones_undefine(map, "selftest-zone", &why));
        (void)remove(map);
    }

    (void)printf("selftest passed\n");
    return 0;
}

static int command_watch(knvr_config *config, int argc, char **argv);
static int command_events(int argc, char **argv);
static int command_prune(knvr_config *config, int argc, char **argv);
static int command_play(int argc, char **argv);
static int command_clip(int argc, char **argv);
static int command_reanalyze(int argc, char **argv);
static int command_objects(int argc, char **argv);
static int command_zones(knvr_config *config, int argc, char **argv);
static int command_zone(knvr_config *config, int argc, char **argv);

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
    } else if (strcmp(command, "view") == 0) {
        knvr_store *store = NULL;
        knvr_view_options view_options;

        knvr_view_options_init(&view_options);
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--render") == 0 && i + 1 < argc) {
                view_options.render = argv[++i];
            } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
                view_options.seconds = atoi(argv[++i]);
            } else if (strcmp(argv[i], "--no-detect") == 0) {
                view_options.detect = false;
            } else if (strcmp(argv[i], "--replay") == 0) {
                view_options.replay = true;
            } else if (argv[i][0] != '-') {
                view_options.camera = argv[i];
            } else {
                knvr_config_close(config);
                return usage(stderr);
            }
        }
        if (!knvr_store_open(&store, NULL)) {
            (void)fprintf(stderr, "kilix-nvr: cannot open the event store\n");
            status = 1;
        } else {
            status = knvr_view(config, store, &view_options);
            knvr_store_close(store);
        }
    } else if (strcmp(command, "review") == 0) {
        knvr_store *store = NULL;

        if (!knvr_store_open(&store, NULL)) {
            (void)fprintf(stderr, "kilix-nvr: cannot open the event store\n");
            status = 1;
        } else {
            status = knvr_review(store);
            knvr_store_close(store);
        }
    } else if (strcmp(command, "play") == 0) {
        status = command_play(argc - 2, argv + 2);
    } else if (strcmp(command, "clip") == 0) {
        status = command_clip(argc - 2, argv + 2);
    } else if (strcmp(command, "reanalyze") == 0) {
        status = command_reanalyze(argc - 2, argv + 2);
    } else if (strcmp(command, "objects") == 0) {
        status = command_objects(argc - 2, argv + 2);
    } else if (strcmp(command, "zones") == 0) {
        status = command_zones(config, argc - 2, argv + 2);
    } else if (strcmp(command, "zone") == 0) {
        status = command_zone(config, argc - 2, argv + 2);
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

/*
 * Milliseconds from a clock that does not step.
 *
 * The tracker ages objects by duration, and time(NULL) can go backwards
 * when ntp corrects the machine - which would make every track either
 * immortal or instantly stale.
 */
static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return (int64_t)time(NULL) * 1000;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int command_watch(knvr_config *config, int argc, char **argv)
{
    knvr_watch_options options;
    knvr_store *store = NULL;
    kod_detector *detector = NULL;
    ksd_listener *sound = NULL;
    knvr_tracker *tracker = NULL;
    knvr_zones *zones = NULL;
    knvr_watch *watch = NULL;
    uint64_t suppressed = 0u;
    int64_t event_id = 0;
    time_t last_motion = 0;
    knvr_watch_stats stats;
    knvr_camera camera;
    knvr_box boxes[KNVR_MOTION_BOX_MAX];
    char url[KRTSP_URL_MAX];
    char mask_path[KNVR_PATH_MAX];
    char zones_path[KNVR_PATH_MAX];
    char log_path[KNVR_PATH_MAX];
    char sound_log[KNVR_PATH_MAX];
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

    {
        knvr_tracker_options track_options;

        knvr_tracker_options_init(&track_options);
        if (!knvr_tracker_create(&tracker, &track_options)) {
            (void)fprintf(stderr,
                          "kilix-nvr: %s: no tracker; detections only\n",
                          name);
        }
    }
    /* Loaded after the camera, because the map has to be reconciled
     * against the geometry the boxes will actually arrive in. */
    if (camera.zones[0] != '\0') {
        char directory[KNVR_PATH_MAX];

        if (knvr_paths_subdir(directory, sizeof(directory), "zones") &&
            snprintf(zones_path, sizeof(zones_path), "%s/%s", directory,
                     camera.zones) > 0) {
            if (!knvr_zones_load(&zones, zones_path,
                                 knvr_watch_width(watch),
                                 knvr_watch_height(watch))) {
                (void)fprintf(stderr,
                              "kilix-nvr: %s: cannot read the zone map; "
                              "no zones\n", name);
            } else {
                (void)printf("  %zu zone%s\n", knvr_zones_count(zones),
                             knvr_zones_count(zones) == 1u ? "" : "s");
            }
        }
    }


    if (camera.sound_events) {
        ksd_options sound_options;
        static char sound_url[KRTSP_URL_MAX];

        ksd_options_init(&sound_options);
        /* Its own log.  Two ffmpegs sharing one file makes "which of them
         * is complaining" a guess, and the audio one complains loudly on
         * a camera that carries no audio at all. */
        if (knvr_paths_state_file(sound_log, sizeof(sound_log),
                                  "ffmpeg-audio.log")) {
            sound_options.log_path = sound_log;
        }
        /* The main stream, which is where the audio is: substreams
         * frequently carry none at all. */
        if (resolve_url(name, false, sound_url, sizeof(sound_url)) &&
            !ksd_open(&sound, sound_url, &sound_options)) {
            (void)fprintf(stderr,
                          "kilix-nvr: %s: no listener; sight only\n", name);
        }
    }

    /* `always` runs it.  `on-view` is blind with no viewer attached, and
     * `cameras` is the one place that says so. */
    if (camera.detect == KNVR_DETECT_ALWAYS) {
        kod_options detector_options;

        kod_options_init(&detector_options);
        /* The square the model is fed, not the frame size: detection runs
         * on crops around what moved, which is cheaper and better at
         * anything small. */
        detector_options.size = 320;
        if (!kod_open(&detector, &detector_options)) {
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
        /*
         * Preclusive zones act here, at the gate, which is where
         * ZoneMinder puts them: movement in a zone marked preclusive is
         * not evidence of anything, so it must not open an event and
         * must not wake the detector.  Dropped regions are counted and
         * reported at the end - a suppression nobody can see is
         * indistinguishable from a camera that stopped working.
         */
        if (zones != NULL && count > 0u) {
            size_t kept = 0u;

            for (size_t b = 0u; b < count; b++) {
                const uint8_t region = knvr_zones_at_point(
                    zones, boxes[b].x + boxes[b].w / 2,
                    boxes[b].y + boxes[b].h - 1);

                if (knvr_zone_is_preclusive(zones, region)) {
                    suppressed++;
                    continue;
                }
                boxes[kept++] = boxes[b];
            }
            count = kept;
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
                kod_box seen[KOD_BOX_MAX];
                kod_rect crops[KOD_REGION_MAX];
                kod_rect moved[KNVR_MOTION_BOX_MAX];
                size_t detections = 0u;
                size_t crop_count;

                /*
                 * Crops around what moved rather than the whole frame,
                 * and the boxes handed in here have already been through
                 * the preclusive-zone filter - so a zone marked as one to
                 * ignore now costs no inference at all, instead of being
                 * filtered after the model has already looked at it.
                 */
                for (size_t b = 0u; b < count && b < KNVR_MOTION_BOX_MAX;
                     b++) {
                    moved[b].x = boxes[b].x;
                    moved[b].y = boxes[b].y;
                    moved[b].w = boxes[b].w;
                    moved[b].h = boxes[b].h;
                }
                crop_count = kod_regions(moved, count,
                                         knvr_watch_width(watch),
                                         knvr_watch_height(watch), 320,
                                         crops, KOD_REGION_MAX, NULL);
                if (kod_detect_regions(detector, frame,
                                       knvr_watch_width(watch),
                                       knvr_watch_height(watch), crops,
                                       crop_count, seen, KOD_BOX_MAX,
                                       &detections)) {
                    for (size_t d = 0u; d < detections; d++) {
                        found[d] = knvr_detect_from(&seen[d]);
                    }
                    /* Zones are resolved per track rather than per
                     * detection, because inertia belongs to the object:
                     * "has been in the drive for three frames" is a fact
                     * about the car, not about the frame. */
                    char zone_of[KNVR_TRACK_MAX][KNVR_STORE_LABEL_MAX];
                    int64_t zone_track[KNVR_TRACK_MAX];
                    size_t zone_count = 0u;

                    (void)knvr_tracker_update(tracker, found, detections,
                                              monotonic_ms());
                    for (size_t t = 0u; t < KNVR_TRACK_MAX; t++) {
                        const knvr_track *object = knvr_tracker_at(tracker, t);
                        knvr_object row;
                        knvr_zone_hit hit;

                        if (object == NULL) {
                            break;
                        }
                        (void)memset(&hit, 0, sizeof(hit));
                        if (zones != NULL) {
                            (void)knvr_zones_track(zones, object->id,
                                                   &object->box,
                                                   monotonic_ms(), &hit);
                        }
                        zone_track[zone_count] = object->id;
                        zone_of[zone_count][0] = '\0';
                        if (hit.settled) {
                            const char *zone_name =
                                knvr_zone_name(zones, hit.region);

                            if (zone_name != NULL) {
                                (void)snprintf(zone_of[zone_count],
                                               KNVR_STORE_LABEL_MAX, "%s",
                                               zone_name);
                            }
                        }
                        zone_count++;
                        if (!object->confirmed) {
                            continue;
                        }
                        (void)memset(&row, 0, sizeof(row));
                        row.event = event_id;
                        row.track = object->id;
                        (void)snprintf(row.camera, sizeof(row.camera), "%s",
                                       name);
                        (void)snprintf(row.label, sizeof(row.label), "%s",
                                       knvr_detect_label(object->class_id));
                        row.score = (double)object->score;
                        row.first_seen = at;
                        row.last_seen = at;
                        row.travelled = object->travelled;
                        row.stationary = object->stationary;
                        (void)snprintf(row.zone, sizeof(row.zone), "%.*s",
                                       KNVR_STORE_LABEL_MAX - 1,
                                       zone_of[zone_count - 1u]);
                        (void)knvr_store_put_object(store, &row);
                    }
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
                        record.track = knvr_tracker_assigned(tracker, d);
                        for (size_t z = 0u; z < zone_count; z++) {
                            if (zone_track[z] == record.track) {
                                (void)snprintf(record.zone,
                                               sizeof(record.zone), "%.*s",
                                               KNVR_STORE_LABEL_MAX - 1,
                                               zone_of[z]);
                                break;
                            }
                        }
                        (void)knvr_store_add_detection(store, &record);
                        (void)printf("    %s %.2f%s%s (track %lld)\n",
                                     record.label, record.score,
                                     record.zone[0] != '\0' ? " in " : "",
                                     record.zone, (long long)record.track);
                    }
                    if (detections > 0u && camera.record != KNVR_RECORD_OFF) {
                        save_still(store, event_id, name, frame,
                                   knvr_watch_width(watch),
                                   knvr_watch_height(watch));
                    }
                } else if (kod_error(detector) != NULL) {
                    (void)fprintf(stderr, "kilix-nvr: %s: %s\n", name,
                                  kod_error(detector));
                    kod_close(detector);
                    detector = NULL;
                }
            }
            /*
             * The first frame with motion, then once more as soon as a
             * track is confirmed.
             *
             * Written twice on purpose: the first motion frame is the one
             * worth looking at when nothing is being tracked, but a
             * tracker needs two sightings before it believes anything, so
             * a single early render could never show a track box and the
             * overlay would look broken.
             */
            if (render != NULL &&
                (rendered == 0 ||
                 (rendered == 1 && knvr_tracker_count(tracker) > 0u &&
                  knvr_tracker_at(tracker, 0u)->confirmed))) {
                uint8_t *copy = malloc((size_t)knvr_watch_width(watch) *
                                       (size_t)knvr_watch_height(watch) * 4u);

                if (copy != NULL) {
                    (void)memcpy(copy, frame,
                                 (size_t)knvr_watch_width(watch) *
                                     (size_t)knvr_watch_height(watch) * 4u);
                    knvr_watch_draw_boxes(copy, knvr_watch_width(watch),
                                          knvr_watch_height(watch), boxes,
                                          count);
                    knvr_track_draw(copy, knvr_watch_width(watch),
                                    knvr_watch_height(watch), tracker);
                    if (write_ppm(render, copy, knvr_watch_width(watch),
                                  knvr_watch_height(watch)) == 0) {
                        (void)printf("  wrote %s%s\n", render,
                                     rendered == 0 ? "" : " (with tracks)");
                        rendered = rendered == 0 ? 1 : 2;
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
        if (sound != NULL) {
            ksd_event heard[8];
            size_t heard_count = 0u;

            if (ksd_step(sound, heard, 8u, &heard_count)) {
                for (size_t h = 0u; h < heard_count; h++) {
                    knvr_detection record;
                    int64_t sound_event = event_id;

                    /* A sound with nothing moving still deserves an
                     * event: a noise in the dark is exactly what a motion
                     * gate cannot see. */
                    if (sound_event == 0 &&
                        knvr_store_event_open(store, name,
                                              KNVR_TRIGGER_SOUND,
                                              (int64_t)time(NULL),
                                              &sound_event)) {
                        event_id = sound_event;
                        last_motion = time(NULL);
                    }
                    (void)memset(&record, 0, sizeof(record));
                    record.event = sound_event;
                    record.at = (int64_t)time(NULL);
                    (void)snprintf(record.label, sizeof(record.label), "%s",
                                   ksd_label(heard[h].class_id));
                    record.score = (double)heard[h].score;
                    (void)knvr_store_add_detection(store, &record);
                    (void)printf("    heard %s %.2f\n", record.label,
                                 record.score);
                }
            } else if (ksd_error(sound) != NULL) {
                /* Once, then sight-only.  A camera with no audio stream
                 * at all reaches here immediately, and repeating it every
                 * second would bury everything else. */
                (void)fprintf(stderr, "kilix-nvr: %s: %s; sight only\n",
                              name, ksd_error(sound));
                ksd_close(sound);
                sound = NULL;
            }
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
    if (tracker != NULL) {
        (void)printf("%lld object%s tracked\n",
                     (long long)knvr_tracker_total(tracker),
                     knvr_tracker_total(tracker) == 1 ? "" : "s");
    }
    if (suppressed > 0u) {
        (void)printf("%llu motion region%s suppressed by preclusive zones\n",
                     (unsigned long long)suppressed,
                     suppressed == 1u ? "" : "s");
    }
    knvr_watch_stop(watch);
    kod_close(detector);
    ksd_close(sound);
    knvr_tracker_free(tracker);
    knvr_zones_free(zones);
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
        } else if (strcmp(argv[i], "--zone") == 0 && i + 1 < argc) {
            (void)snprintf(query.zone, sizeof(query.zone), "%s", argv[++i]);
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

/* --------------------- clips, playback and reanalyze --------------------- */

/*
 * The segment covering a moment, or empty.
 *
 * Segments are named by their start time, so the one that contains an
 * event is the newest whose name is not after it.  Reading the directory
 * beats indexing them: the segmenter writes files without telling anyone,
 * and an index that can disagree with the disk is worse than no index.
 */
static bool segment_covering(const char *camera, int64_t at, char *out,
                             size_t size)
{
    char directory[KNVR_PATH_MAX];
    char segments[KNVR_PATH_MAX];
    DIR *handle;
    struct dirent *entry;
    time_t best = 0;
    bool found = false;

    if (!knvr_paths_subdir(segments, sizeof(segments), "segments")) {
        return false;
    }
    if (snprintf(directory, sizeof(directory), "%s/%s", segments, camera) < 0) {
        return false;
    }
    handle = opendir(directory);
    if (handle == NULL) {
        return false;
    }
    while ((entry = readdir(handle)) != NULL) {
        char candidate[KNVR_PATH_MAX];
        struct stat info;

        if (entry->d_name[0] == '.') {
            continue;
        }
        if (snprintf(candidate, sizeof(candidate), "%s/%s", directory,
                     entry->d_name) < 0) {
            continue;
        }
        if (stat(candidate, &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }
        /* mtime is when the segment was closed, so a segment covers the
         * window ending there.  Its start is what matters, and the
         * closest one at or before the event is the answer. */
        if (info.st_mtime <= (time_t)at + 90 && info.st_mtime > best) {
            best = info.st_mtime;
            (void)snprintf(out, size, "%s", candidate);
            found = true;
        }
    }
    (void)closedir(handle);
    return found;
}

static int command_play(int argc, char **argv)
{
    knvr_store *store = NULL;
    knvr_query query;
    knvr_event events[64];
    knvr_media media[8];
    size_t count = 0u;
    size_t media_count = 0u;
    int64_t wanted;
    char segment[KNVR_PATH_MAX];
    const char *target = NULL;

    if (argc != 1) {
        return usage(stderr);
    }
    wanted = atoll(argv[0]);
    if (!knvr_store_open(&store, NULL)) {
        (void)fprintf(stderr, "kilix-nvr: cannot open the event store\n");
        return 1;
    }
    knvr_query_init(&query);
    query.limit = 64;
    if (!knvr_store_events(store, &query, events, 64u, &count)) {
        knvr_store_close(store);
        return 1;
    }
    for (size_t i = 0u; i < count && i < 64u; i++) {
        if (events[i].id != wanted) {
            continue;
        }
        /* A clip belonging to the event wins; otherwise the segment that
         * was recording when it happened. */
        if (knvr_store_media(store, wanted, media, 8u, &media_count)) {
            for (size_t m = 0u; m < media_count && m < 8u; m++) {
                if (strcmp(media[m].kind, "clip") == 0 ||
                    strcmp(media[m].kind, "segment") == 0) {
                    target = media[m].path;
                    break;
                }
            }
        }
        if (target == NULL &&
            segment_covering(events[i].camera, events[i].started, segment,
                             sizeof(segment))) {
            target = segment;
        }
        break;
    }
    knvr_store_close(store);
    if (target == NULL) {
        (void)fprintf(stderr,
                      "kilix-nvr: no footage for event %lld\n",
                      (long long)wanted);
        return 1;
    }
    (void)printf("%s\n", target);
    return 0;
}

/*
 * Cut an event out of the segments it spans, without re-encoding.
 *
 * -c copy, so a clip costs I/O and keeps the camera's own bitstream.  The
 * start is ten seconds before the trigger, which is the pre-roll the
 * design promised and which needs no buffering because the footage was
 * already being written.
 */
static int command_clip(int argc, char **argv)
{
    knvr_store *store = NULL;
    knvr_query query;
    knvr_event events[64];
    size_t count = 0u;
    int64_t wanted;
    char segment[KNVR_PATH_MAX];
    char output[KNVR_PATH_MAX];
    char clips[KNVR_PATH_MAX];
    const knvr_event *event = NULL;
    int status = 1;

    if (argc != 1) {
        return usage(stderr);
    }
    wanted = atoll(argv[0]);
    if (!knvr_store_open(&store, NULL)) {
        return 1;
    }
    knvr_query_init(&query);
    query.limit = 64;
    (void)knvr_store_events(store, &query, events, 64u, &count);
    for (size_t i = 0u; i < count && i < 64u; i++) {
        if (events[i].id == wanted) {
            event = &events[i];
            break;
        }
    }
    if (event == NULL ||
        !segment_covering(event->camera, event->started, segment,
                          sizeof(segment))) {
        (void)fprintf(stderr, "kilix-nvr: no footage for event %lld\n",
                      (long long)wanted);
        knvr_store_close(store);
        return 1;
    }
    if (!knvr_paths_subdir(clips, sizeof(clips), "clips") ||
        snprintf(output, sizeof(output), "%s/%s-%lld.mkv", clips,
                 event->camera, (long long)wanted) < 0) {
        knvr_store_close(store);
        return 1;
    }
    {
        struct stat info;
        pid_t child;
        int wait_status = 0;
        long offset = 0;
        char start[32];
        char duration[32];

        if (stat(segment, &info) == 0) {
            /* Ten seconds of pre-roll, clamped at the segment's start. */
            const long into = (long)(event->started - info.st_mtime);

            offset = into > 10 ? into - 10 : 0;
        }
        (void)snprintf(start, sizeof(start), "%ld", offset);
        (void)snprintf(duration, sizeof(duration), "%lld",
                       (long long)((event->ended > event->started
                                        ? event->ended - event->started
                                        : 10) + 10));
        child = fork();
        if (child == 0) {
            (void)execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel",
                         "error", "-nostdin", "-ss", start, "-i", segment,
                         "-t", duration, "-c", "copy", "-y", output,
                         (char *)NULL);
            _exit(127);
        }
        if (child > 0 && waitpid(child, &wait_status, 0) == child &&
            WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0) {
            (void)knvr_store_add_media(store, wanted, "clip", output);
            (void)printf("%s\n", output);
            status = 0;
        } else {
            (void)fprintf(stderr, "kilix-nvr: could not cut the clip\n");
        }
    }
    knvr_store_close(store);
    return status;
}

/*
 * Run the detector over an event's stored still again.
 *
 * The reason this exists is a better model arriving after the footage
 * did: re-analysing is how yesterday's events get today's answers,
 * without keeping the original frames in memory for a day.
 */
static int command_reanalyze(int argc, char **argv)
{
    knvr_store *store = NULL;
    kod_detector *detector = NULL;
    kod_options options;
    knvr_media media[8];
    kod_box seen[KOD_BOX_MAX];
    sr_canvas still;
    size_t media_count = 0u;
    size_t detections = 0u;
    int64_t wanted;
    const char *path = NULL;
    uint8_t *bgra = NULL;
    int status = 1;

    if (argc != 1) {
        return usage(stderr);
    }
    wanted = atoll(argv[0]);
    if (!knvr_store_open(&store, NULL)) {
        return 1;
    }
    if (!knvr_store_media(store, wanted, media, 8u, &media_count)) {
        knvr_store_close(store);
        return 1;
    }
    for (size_t i = 0u; i < media_count && i < 8u; i++) {
        if (strcmp(media[i].kind, "still") == 0) {
            path = media[i].path;
            break;
        }
    }
    if (path == NULL || !sr_load_ppm(&still, path)) {
        (void)fprintf(stderr,
                      "kilix-nvr: event %lld has no still to re-read\n",
                      (long long)wanted);
        knvr_store_close(store);
        return 1;
    }
    kod_options_init(&options);
    /* A still has no motion to crop to, so the whole frame is the only
     * honest thing to look at - and it is why this is worth doing at all:
     * yesterday's frames get today's model. */
    options.size = 320;
    if (!kod_open(&detector, &options)) {
        (void)fprintf(stderr, "kilix-nvr: no detector\n");
        sr_canvas_free(&still);
        knvr_store_close(store);
        return 1;
    }
    bgra = malloc((size_t)still.w * (size_t)still.h * 4u);
    if (bgra != NULL) {
        for (int i = 0; i < still.w * still.h; i++) {
            const uint32_t pixel = still.px[i];

            bgra[i * 4 + 0] = (uint8_t)(pixel & 0xFFu);
            bgra[i * 4 + 1] = (uint8_t)((pixel >> 8) & 0xFFu);
            bgra[i * 4 + 2] = (uint8_t)((pixel >> 16) & 0xFFu);
            bgra[i * 4 + 3] = 0xFF;
        }
        if (kod_detect(detector, bgra, still.w, still.h, seen, KOD_BOX_MAX,
                       &detections)) {
            for (size_t d = 0u; d < detections; d++) {
                const knvr_detection_box box = knvr_detect_from(&seen[d]);
                knvr_detection record;

                (void)memset(&record, 0, sizeof(record));
                record.event = wanted;
                record.at = (int64_t)time(NULL);
                (void)snprintf(record.label, sizeof(record.label), "%s",
                               knvr_detect_label(box.class_id));
                record.score = (double)box.score;
                record.x = box.x;
                record.y = box.y;
                record.w = box.w;
                record.h = box.h;
                (void)knvr_store_add_detection(store, &record);
                (void)printf("%s %.2f\n", record.label, record.score);
            }
            if (detections == 0u) {
                (void)printf("nothing found\n");
            }
            status = 0;
        }
        free(bgra);
    }
    kod_close(detector);
    sr_canvas_free(&still);
    knvr_store_close(store);
    return status;
}

/* ------------------------- objects and zones ----------------------------- */

static int command_objects(int argc, char **argv)
{
    knvr_store *store = NULL;
    knvr_object objects[64];
    size_t count = 0u;
    int64_t wanted;

    if (argc != 1) {
        return usage(stderr);
    }
    wanted = atoll(argv[0]);
    if (!knvr_store_open(&store, NULL)) {
        (void)fprintf(stderr, "kilix-nvr: cannot open the event store\n");
        return 1;
    }
    if (!knvr_store_objects(store, wanted, objects, 64u, &count)) {
        knvr_store_close(store);
        return 1;
    }
    if (count == 0u) {
        (void)printf("event %lld tracked nothing\n", (long long)wanted);
        knvr_store_close(store);
        return 0;
    }
    (void)printf("%-6s %-10s %-6s %-9s %-8s %s\n", "TRACK", "WHAT", "SCORE",
                 "SECONDS", "MOVED", "ZONES");
    for (size_t i = 0u; i < count && i < 64u; i++) {
        const knvr_object *object = &objects[i];

        (void)printf("%-6lld %-10s %-6.2f %-9lld %-8d %s%s\n",
                     (long long)object->track, object->label, object->score,
                     (long long)(object->last_seen - object->first_seen),
                     object->travelled,
                     object->zone[0] != '\0' ? object->zone : "-",
                     object->stationary ? "  (parked)" : "");
    }
    knvr_store_close(store);
    return 0;
}

/*
 * Where a camera's zone map lives.
 *
 * One map per camera, named after it, so `zone add` needs no path and a
 * map cannot end up attached to the wrong camera.
 */
static bool zone_map_path(const char *camera, char *out, size_t size)
{
    char directory[KNVR_PATH_MAX];

    if (!knvr_paths_subdir(directory, sizeof(directory), "zones")) {
        return false;
    }
    return snprintf(out, size, "%s/%s.png", directory, camera) > 0;
}

/*
 * One frame from a camera, for sizing or painting a zone map.
 *
 * `ppm` may be NULL when only the geometry is wanted.  Fifteen seconds:
 * the connection grace alone is ten, and a camera that has not produced a
 * frame by then is not going to during this command.
 */
static bool grab_frame(const char *camera, const char *ppm, int *width,
                       int *height)
{
    knvr_watch_options options;
    knvr_watch *watch = NULL;
    char url[KRTSP_URL_MAX];
    char log_path[KNVR_PATH_MAX];
    time_t deadline;
    bool ok = false;

    if (!resolve_url(camera, true, url, sizeof(url))) {
        (void)fprintf(stderr,
                      "kilix-nvr: no stream URL for '%s' in cameras.conf\n",
                      camera);
        return false;
    }
    knvr_watch_options_init(&options);
    if (knvr_paths_state_file(log_path, sizeof(log_path), "ffmpeg.log")) {
        options.log_path = log_path;
    }
    if (!knvr_watch_start(&watch, url, &options)) {
        (void)fprintf(stderr, "kilix-nvr: %s: cannot reach the camera\n",
                      camera);
        knvr_watch_stop(watch);
        return false;
    }
    deadline = time(NULL) + 15;
    while (time(NULL) < deadline) {
        const uint8_t *frame = NULL;
        knvr_box boxes[KNVR_MOTION_BOX_MAX];
        size_t count = 0u;
        struct timespec pause = {0, 100 * 1000 * 1000};

        if (!knvr_watch_step(watch, &frame, boxes, KNVR_MOTION_BOX_MAX,
                             &count)) {
            (void)nanosleep(&pause, NULL);
            continue;
        }
        *width = knvr_watch_width(watch);
        *height = knvr_watch_height(watch);
        ok = ppm == NULL || write_ppm(ppm, frame, *width, *height) == 0;
        break;
    }
    if (!ok) {
        (void)fprintf(stderr, "kilix-nvr: %s: no frame arrived\n", camera);
    }
    knvr_watch_stop(watch);
    return ok;
}

static int command_zones(knvr_config *config, int argc, char **argv)
{
    knvr_camera camera;
    knvr_zones *zones = NULL;
    char path[KNVR_PATH_MAX];

    if (argc != 1) {
        return usage(stderr);
    }
    if (!knvr_config_get(config, argv[0], &camera)) {
        (void)fprintf(stderr, "kilix-nvr: no camera named '%s'\n", argv[0]);
        return 1;
    }
    if (!zone_map_path(argv[0], path, sizeof(path))) {
        return 1;
    }
    /* Listing does not need the camera, so the geometry here is nominal:
     * cells and names are what is being reported, not positions. */
    if (!knvr_zones_load(&zones, path, 1000, 1000)) {
        (void)printf("no zones for %s; make one with `kilix-nvr zone add "
                     "%s <zone>`\n", argv[0], argv[0]);
        return 0;
    }
    if (camera.zones[0] == '\0') {
        (void)printf("note: %s has a zone map but zones= is unset, so "
                     "nothing reads it\n", argv[0]);
    }
    (void)printf("%-3s %-16s %-8s %-11s %-8s %s\n", "ID", "ZONE", "INERTIA",
                 "PRECLUSIVE", "LOITER", "CELLS");
    for (size_t i = 0u; i < knvr_zones_count(zones); i++) {
        const knvr_zone *zone = knvr_zones_at(zones, i);
        char loiter[16];

        if (zone->loiter_seconds > 0) {
            (void)snprintf(loiter, sizeof(loiter), "%ds",
                           zone->loiter_seconds);
        } else {
            (void)snprintf(loiter, sizeof(loiter), "-");
        }
        (void)printf("%-3u %-16s %-8d %-11s %-8s %zu\n", zone->region,
                     zone->name, zone->inertia,
                     zone->preclusive ? "yes" : "no", loiter, zone->cells);
        if (zone->cells == 0u) {
            (void)printf("    nothing painted yet: `kilix-nvr zone paint "
                         "%s`, then press %u\n", argv[0], zone->region);
        }
    }
    knvr_zones_free(zones);
    return 0;
}

static int command_zone(knvr_config *config, int argc, char **argv)
{
    knvr_camera camera;
    char path[KNVR_PATH_MAX];
    const char *action;
    const char *name;

    if (argc < 2) {
        return usage(stderr);
    }
    action = argv[0];
    name = argv[1];
    if (!knvr_config_get(config, name, &camera)) {
        (void)fprintf(stderr, "kilix-nvr: no camera named '%s'\n", name);
        return 1;
    }
    if (!zone_map_path(name, path, sizeof(path))) {
        return 1;
    }

    if (strcmp(action, "paint") == 0) {
        char frame[KNVR_PATH_MAX];
        char directory[KNVR_PATH_MAX];
        int width = 0;
        int height = 0;

        if (argc != 2) {
            return usage(stderr);
        }
        if (!knvr_paths_subdir(directory, sizeof(directory), "zones") ||
            snprintf(frame, sizeof(frame), "%s/%s-frame.ppm", directory,
                     name) < 0) {
            return 1;
        }
        if (!grab_frame(name, frame, &width, &height)) {
            return 1;
        }
        (void)printf("painting %s over a %dx%d frame\n", path, width, height);
        /* Handing over rather than shelling out: the editor owns the
         * terminal from here, and a wrapper process sitting in the middle
         * of a full-screen graphical program is a wrapper that breaks its
         * input. */
        (void)execlp("kilix-mask", "kilix-mask", "--image", frame, path,
                     (char *)NULL);
        (void)fprintf(stderr,
                      "kilix-nvr: kilix-mask is not installed; run "
                      "`kilix install kilix-mask`\n");
        return 1;
    }

    if (strcmp(action, "remove") == 0) {
        const char *reason = NULL;

        if (argc != 3) {
            return usage(stderr);
        }
        if (!knvr_zones_undefine(path, argv[2], &reason)) {
            (void)fprintf(stderr, "kilix-nvr: %s\n",
                          reason != NULL ? reason : "cannot remove that zone");
            return 1;
        }
        (void)printf("%s: removed zone %s\n", name, argv[2]);
        return 0;
    }

    if (strcmp(action, "add") == 0) {
        const char *reason = NULL;
        uint8_t region = 0u;
        int inertia = 1;
        int loiter = 0;
        bool preclusive = false;
        int width = 0;
        int height = 0;
        struct stat existing;

        if (argc < 3) {
            return usage(stderr);
        }
        for (int i = 3; i < argc; i++) {
            if (strncmp(argv[i], "inertia=", 8) == 0) {
                inertia = atoi(argv[i] + 8);
            } else if (strncmp(argv[i], "loiter=", 7) == 0) {
                loiter = atoi(argv[i] + 7);
            } else if (strcmp(argv[i], "preclusive=yes") == 0) {
                preclusive = true;
            } else if (strcmp(argv[i], "preclusive=no") == 0) {
                preclusive = false;
            } else {
                (void)fprintf(stderr, "kilix-nvr: %s: unknown zone setting\n",
                              argv[i]);
                return 1;
            }
        }
        if (inertia < 1 || inertia > 1000 || loiter < 0 || loiter > 86400) {
            (void)fprintf(stderr,
                          "kilix-nvr: inertia is 1 to 1000, loiter 0 to "
                          "86400\n");
            return 1;
        }
        /* A new map has to be the size of what it describes, and only the
         * camera can say what that is. */
        if (stat(path, &existing) != 0 &&
            !grab_frame(name, NULL, &width, &height)) {
            return 1;
        }
        if (!knvr_zones_define(path, argv[2], width, height, inertia,
                               preclusive, loiter, &region, &reason)) {
            (void)fprintf(stderr, "kilix-nvr: %s\n",
                          reason != NULL ? reason : "cannot add that zone");
            return 1;
        }
        /* Attaching it too, because a zone map the camera does not read
         * is the one mistake this command exists to prevent. */
        if (camera.zones[0] == '\0') {
            (void)snprintf(camera.zones, sizeof(camera.zones), "%s.png",
                           name);
            if (!knvr_config_put(config, &camera)) {
                (void)fprintf(stderr,
                              "kilix-nvr: zone added but %s could not be "
                              "pointed at it\n", name);
                return 1;
            }
        }
        (void)printf("%s: zone %s is region %u; paint it with "
                     "`kilix-nvr zone paint %s` and press %u\n", name,
                     argv[2], region, name, region);
        return 0;
    }
    return usage(stderr);
}
