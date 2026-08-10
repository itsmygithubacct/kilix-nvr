/*
 * kilix-nvr — a terminal-native network video recorder.
 *
 * What a camera does is its configuration, not an invocation: there is no
 * `record` command and no `detect` command.  `add` onboards one with every
 * capability off, `set` turns them on, `cameras` says what each is doing,
 * and `run` honours it.
 */

#include "knvr_config.h"
#include "knvr_paths.h"
#include "knvr_watch.h"

#include "kilix_rtsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        "                          decode one camera and report what moves\n"
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

static int command_watch(knvr_config *config, int argc, char **argv)
{
    knvr_watch_options options;
    knvr_watch *watch = NULL;
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
            (void)printf("  motion: %zu region%s\n", count,
                         count == 1u ? "" : "s");
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
        }
        {
            struct timespec pause = {0, 30 * 1000 * 1000};

            (void)nanosleep(&pause, NULL);
        }
    }
    knvr_watch_get_stats(watch, &stats);
    (void)printf("%llu frames, %llu with motion, %llu boxes, newest %dms old\n",
                 (unsigned long long)stats.frames,
                 (unsigned long long)stats.motion_frames,
                 (unsigned long long)stats.boxes, stats.last_age_ms);
    knvr_watch_stop(watch);
    return 0;
}
