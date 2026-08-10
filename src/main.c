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

#include "kilix_rtsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
