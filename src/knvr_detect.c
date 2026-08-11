/*
 * Talking to the detector.
 *
 * Write one frame, read 480 bytes.  Both sides are fixed size, so the
 * framing is the sizes themselves - there is no header, no delimiter and
 * nothing to parse, which is also why a desynchronised pipe is impossible
 * rather than merely unlikely.
 *
 * The process is supervised the way ffmpeg is: a death is a degradation,
 * not a fault.  A camera whose detector has died is motion-only until it
 * comes back, because the alternative - taking the pipeline down - loses
 * the recording as well as the detection.
 */

#include "knvr_detect.h"
#include "knvr_command.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ERROR_MAX 160

/* Words the caller may give in the detector command, before --geometry
 * and its value are appended.  A cap that truncated rather than refused
 * would run a different command than the one asked for. */
#define KNVR_DETECT_ARGV_MAX 13

struct knvr_detector {
    pid_t child;
    int to_child;
    int from_child;
    int width;
    int height;
    size_t frame_bytes;
    float min_score;
    int timeout_seconds;
    bool broken;
    char error[ERROR_MAX];
};

/*
 * The classes this reports, by COCO id.
 *
 * An allowlist, not a threshold, is the defence against nonsense: the
 * models invented toilets and aeroplanes on still footage, and none of
 * those are here.  That is what makes a 0.25 confidence safe - the
 * measured alternative, 0.45, dropped real people in infrared.
 */
static const struct {
    int id;
    const char *label;
} ALLOWED[] = {
    {0, "person"},
    {1, "bicycle"},
    {2, "car"},
    {3, "motorcycle"},
    {5, "bus"},
    {7, "truck"},
    {14, "bird"},
    {15, "cat"},
    {16, "dog"},
    {17, "horse"},
    {18, "sheep"},
    {19, "cow"},
    {21, "bear"},
    {24, "backpack"},
    {26, "handbag"},
    {28, "suitcase"}
};

const char *knvr_detect_label(int class_id)
{
    for (size_t i = 0u; i < sizeof(ALLOWED) / sizeof(ALLOWED[0]); i++) {
        if (ALLOWED[i].id == class_id) {
            return ALLOWED[i].label;
        }
    }
    return NULL;
}

static bool fail(knvr_detector *detector, const char *reason)
{
    if (detector != NULL) {
        (void)snprintf(detector->error, sizeof(detector->error), "%s",
                       reason);
        detector->broken = true;
    }
    return false;
}

const char *knvr_detector_error(const knvr_detector *detector)
{
    if (detector == NULL || detector->error[0] == '\0') {
        return NULL;
    }
    return detector->error;
}

void knvr_detector_options_init(knvr_detector_options *options)
{
    if (options == NULL) {
        return;
    }
    (void)memset(options, 0, sizeof(*options));
    options->width = 640;
    options->height = 360;
    /* Measured.  See the allowlist above for why this is not reckless. */
    options->min_score = 0.25f;
    options->timeout_seconds = 5;
}

bool knvr_detector_start(
    knvr_detector **out, const knvr_detector_options *options)
{
    const char *default_argv[] = {NULL, NULL};
    char bundled[1024];
    const char *env_argv[KNVR_DETECT_ARGV_MAX + 1];
    char env_storage[512];
    const char *const *chosen;
    knvr_detector_options defaults;
    knvr_detector *detector;
    int to_child[2];
    int from_child[2];
    char geometry[32];

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    if (options == NULL) {
        knvr_detector_options_init(&defaults);
        options = &defaults;
    }
    ksd_bundled_tool("kilix-nvr-detect", bundled, sizeof(bundled));
    default_argv[0] = bundled;
    chosen = options->argv;
    if (chosen == NULL &&
        knvr_command_from_env("KILIX_NVR_DETECT", env_storage,
                              sizeof(env_storage), env_argv,
                              KNVR_DETECT_ARGV_MAX + 1u)) {
        chosen = env_argv;
    }
    if (chosen == NULL) {
        chosen = default_argv;
    }
    if (options->width <= 0 || options->height <= 0) {
        return false;
    }
    detector = calloc(1u, sizeof(*detector));
    if (detector == NULL) {
        return false;
    }
    detector->width = options->width;
    detector->height = options->height;
    detector->frame_bytes = (size_t)options->width * (size_t)options->height *
                            4u;
    detector->min_score = options->min_score;
    detector->timeout_seconds =
        options->timeout_seconds > 0 ? options->timeout_seconds : 5;
    detector->to_child = -1;
    detector->from_child = -1;

    if (pipe(to_child) != 0) {
        free(detector);
        return false;
    }
    if (pipe(from_child) != 0) {
        (void)close(to_child[0]);
        (void)close(to_child[1]);
        free(detector);
        return false;
    }
    (void)snprintf(geometry, sizeof(geometry), "%dx%d", options->width,
                   options->height);

    detector->child = fork();
    if (detector->child < 0) {
        (void)close(to_child[0]); (void)close(to_child[1]);
        (void)close(from_child[0]); (void)close(from_child[1]);
        free(detector);
        return false;
    }
    if (detector->child == 0) {
        const char *const *argv = chosen;
        char *child_argv[KNVR_DETECT_ARGV_MAX + 3];
        size_t count = 0u;

        (void)dup2(to_child[0], STDIN_FILENO);
        (void)dup2(from_child[1], STDOUT_FILENO);
        (void)close(to_child[0]); (void)close(to_child[1]);
        (void)close(from_child[0]); (void)close(from_child[1]);
        /* stderr is left alone: the detector's diagnostics are the
         * caller's business, and swallowing them makes a model that
         * failed to load indistinguishable from one that sees nothing. */
        while (argv[count] != NULL && count < KNVR_DETECT_ARGV_MAX) {
            child_argv[count] = (char *)argv[count];
            count++;
        }
        child_argv[count++] = (char *)"--geometry";
        child_argv[count++] = geometry;
        child_argv[count] = NULL;
        (void)execvp(child_argv[0], child_argv);
        _exit(127);
    }
    (void)close(to_child[0]);
    (void)close(from_child[1]);
    detector->to_child = to_child[1];
    detector->from_child = from_child[0];
    /* A detector that dies mid-write must not kill this process. */
    (void)signal(SIGPIPE, SIG_IGN);
    *out = detector;
    return true;
}

void knvr_detector_stop(knvr_detector *detector)
{
    int status;

    if (detector == NULL) {
        return;
    }
    if (detector->to_child >= 0) {
        (void)close(detector->to_child);
    }
    if (detector->from_child >= 0) {
        (void)close(detector->from_child);
    }
    if (detector->child > 0) {
        /* Closing stdin is the polite exit; the signal is for a model
         * that is mid-inference and will not notice EOF for a while. */
        (void)kill(detector->child, SIGTERM);
        (void)waitpid(detector->child, &status, 0);
    }
    free(detector);
}

static bool write_all(knvr_detector *detector, const uint8_t *bytes,
                      size_t size)
{
    size_t offset = 0u;

    while (offset < size) {
        const ssize_t written =
            write(detector->to_child, bytes + offset, size - offset);

        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return fail(detector, "the detector stopped reading");
    }
    return true;
}

static bool read_exactly(knvr_detector *detector, uint8_t *bytes, size_t size)
{
    size_t offset = 0u;

    while (offset < size) {
        struct pollfd descriptor = {detector->from_child, POLLIN, 0};
        const int ready =
            poll(&descriptor, 1u, detector->timeout_seconds * 1000);
        ssize_t got;

        if (ready == 0) {
            /* A wedged model is indistinguishable from a slow one, so the
             * timeout is what turns "no answer" into a decision. */
            return fail(detector, "the detector did not answer in time");
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(detector, "the detector could not be polled");
        }
        got = read(detector->from_child, bytes + offset, size - offset);
        if (got > 0) {
            offset += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        return fail(detector, "the detector stopped answering");
    }
    return true;
}

bool knvr_detector_run(
    knvr_detector *detector, const uint8_t *frame,
    knvr_detection_box *boxes, size_t capacity, size_t *count)
{
    uint8_t reply[KNVR_DETECT_BYTES];
    float rows[KNVR_DETECT_ROWS][KNVR_DETECT_COLUMNS];
    size_t written = 0u;

    if (count != NULL) {
        *count = 0u;
    }
    if (detector == NULL || frame == NULL) {
        return false;
    }
    if (detector->broken) {
        return false;
    }
    if (!write_all(detector, frame, detector->frame_bytes)) {
        return false;
    }
    if (!read_exactly(detector, reply, sizeof(reply))) {
        return false;
    }
    (void)memcpy(rows, reply, sizeof(rows));

    for (size_t i = 0u; i < KNVR_DETECT_ROWS && written < capacity; i++) {
        const int class_id = (int)rows[i][0];
        const float score = rows[i][1];

        /* Rows are score-ordered, so the first one below the threshold
         * ends the useful part of the reply. */
        if (score < detector->min_score) {
            break;
        }
        if (knvr_detect_label(class_id) == NULL) {
            continue;   /* outside the allowlist: not ours to report */
        }
        {
            /* Normalised y0,x0,y1,x1 - note the order - into frame
             * pixels, once, here. */
            const float y0 = rows[i][2];
            const float x0 = rows[i][3];
            const float y1 = rows[i][4];
            const float x1 = rows[i][5];
            int left = (int)(x0 * (float)detector->width);
            int top = (int)(y0 * (float)detector->height);
            int right = (int)(x1 * (float)detector->width);
            int bottom = (int)(y1 * (float)detector->height);

            if (left < 0) { left = 0; }
            if (top < 0) { top = 0; }
            if (right > detector->width) { right = detector->width; }
            if (bottom > detector->height) { bottom = detector->height; }
            if (right <= left || bottom <= top) {
                continue;
            }
            boxes[written].class_id = class_id;
            boxes[written].score = score;
            boxes[written].x = left;
            boxes[written].y = top;
            boxes[written].w = right - left;
            boxes[written].h = bottom - top;
            written++;
        }
    }
    if (count != NULL) {
        *count = written;
    }
    return true;
}
