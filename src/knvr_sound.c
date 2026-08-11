/*
 * Audio in, sound events out.
 *
 * Two subprocesses: an ffmpeg pulling audio off the camera, and the
 * classifier reading fixed windows from us.  Both are supervised the same
 * way the video path supervises its own, and both dying is a degradation
 * rather than a fault - a camera that stops hearing should keep seeing.
 */

#include "knvr_sound.h"
#include "knvr_command.h"
#include "knvr_detect.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#define ERROR_MAX 160

/*
 * Words in the listener command.  Enough for a venv python, the tool, a
 * model path and a threshold; longer than that is refused before the
 * fork rather than truncated, because a silently shortened command line
 * runs *something*, and working out which is nobody's afternoon.
 */
#define KNVR_SOUND_ARGV_MAX 15

struct knvr_sound {
    pid_t ffmpeg;
    pid_t classifier;
    int audio_fd;      /* PCM from ffmpeg */
    int to_child;
    int from_child;
    float min_score;
    bool broken;
    int16_t window[KNVR_SOUND_WINDOW];
    size_t filled;
    char error[ERROR_MAX];
};

/*
 * What a camera is worth reporting for.  An allowlist for the same reason
 * the object detector has one: the models will happily name a hundred
 * classes and almost none of them are a reason to look at a recording.
 */
static const struct {
    int id;
    const char *label;
} ALLOWED[] = {
    {0, "speech"},
    {1, "shout"},
    {2, "dog"},
    {3, "glass"},
    {4, "alarm"},
    {5, "siren"},
    {6, "gunshot"},
    {7, "vehicle"},
    {8, "knock"}
};

const char *knvr_sound_label(int class_id)
{
    for (size_t i = 0u; i < sizeof(ALLOWED) / sizeof(ALLOWED[0]); i++) {
        if (ALLOWED[i].id == class_id) {
            return ALLOWED[i].label;
        }
    }
    return NULL;
}

static bool fail(knvr_sound *sound, const char *reason)
{
    if (sound != NULL) {
        (void)snprintf(sound->error, sizeof(sound->error), "%s", reason);
        sound->broken = true;
    }
    return false;
}

const char *knvr_sound_error(const knvr_sound *sound)
{
    if (sound == NULL || sound->error[0] == '\0') {
        return NULL;
    }
    return sound->error;
}

void knvr_sound_options_init(knvr_sound_options *options)
{
    if (options == NULL) {
        return;
    }
    (void)memset(options, 0, sizeof(*options));
    options->min_score = 0.5f;
}

/* ffmpeg pulling audio only: -vn, so nothing is decoded that is not
 * wanted, and a fixed rate so the window is a fixed number of bytes. */
static pid_t spawn_ffmpeg(const char *url, const char *log_path, int *out_fd)
{
    int pipes[2];
    pid_t child;

    if (pipe(pipes) != 0) {
        return -1;
    }
    child = fork();
    if (child < 0) {
        (void)close(pipes[0]);
        (void)close(pipes[1]);
        return -1;
    }
    if (child == 0) {
        char rate[16];
        char *argv[20];
        size_t count = 0u;

        (void)snprintf(rate, sizeof(rate), "%d", KNVR_SOUND_RATE);
        (void)dup2(pipes[1], STDOUT_FILENO);
        (void)close(pipes[0]);
        (void)close(pipes[1]);
        if (log_path != NULL) {
            const int log = open(log_path, O_WRONLY | O_CREAT | O_APPEND,
                                 0600);

            if (log >= 0) {
                (void)dup2(log, STDERR_FILENO);
                (void)close(log);
            }
        }
        argv[count++] = (char *)"ffmpeg";
        argv[count++] = (char *)"-hide_banner";
        argv[count++] = (char *)"-loglevel";
        argv[count++] = (char *)"warning";
        argv[count++] = (char *)"-nostdin";
        /*
         * Only for an RTSP input.  ffmpeg refuses a demuxer option the
         * demuxer does not have - "Option rtsp_transport not found" and
         * exit 1 - so passing it unconditionally would mean this path
         * could never read anything but a camera.  Being able to point it
         * at a file is what makes the whole audio path testable without
         * one.
         */
        if (strncmp(url, "rtsp://", 7) == 0) {
            argv[count++] = (char *)"-rtsp_transport";
            argv[count++] = (char *)"tcp";
        }
        argv[count++] = (char *)"-i";
        argv[count++] = (char *)url;
        argv[count++] = (char *)"-vn";
        argv[count++] = (char *)"-f";
        argv[count++] = (char *)"s16le";
        argv[count++] = (char *)"-ar";
        argv[count++] = rate;
        argv[count++] = (char *)"-ac";
        argv[count++] = (char *)"1";
        argv[count++] = (char *)"-";
        argv[count] = NULL;
        (void)execvp("ffmpeg", argv);
        _exit(127);
    }
    (void)close(pipes[1]);
    *out_fd = pipes[0];
    return child;
}

bool knvr_sound_start(
    knvr_sound **out, const char *url, const knvr_sound_options *options)
{
    const char *default_argv[] = {NULL, NULL};
    char bundled[1024];
    const char *env_argv[KNVR_SOUND_ARGV_MAX + 1];
    char env_storage[512];
    const char *const *chosen;
    knvr_sound_options defaults;
    knvr_sound *sound;
    int to_child[2];
    int from_child[2];

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    if (url == NULL || url[0] == '\0') {
        return false;
    }
    if (options == NULL) {
        knvr_sound_options_init(&defaults);
        options = &defaults;
    }
    sound = calloc(1u, sizeof(*sound));
    if (sound == NULL) {
        return false;
    }
    sound->min_score = options->min_score;
    sound->audio_fd = -1;
    sound->to_child = -1;
    sound->from_child = -1;

    if (pipe(to_child) != 0 || pipe(from_child) != 0) {
        free(sound);
        return false;
    }
    knvr_command_bundled("kilix-nvr-listen", bundled, sizeof(bundled));
    default_argv[0] = bundled;
    chosen = options->argv;
    if (chosen == NULL &&
        knvr_command_from_env("KILIX_NVR_LISTEN", env_storage,
                              sizeof(env_storage), env_argv,
                              KNVR_SOUND_ARGV_MAX + 1u)) {
        chosen = env_argv;
    }
    if (chosen == NULL) {
        chosen = default_argv;
    }
    {
        size_t words = 0u;

        while (chosen[words] != NULL) {
            words++;
        }
        if (words == 0u || words > KNVR_SOUND_ARGV_MAX) {
            (void)close(to_child[0]); (void)close(to_child[1]);
            (void)close(from_child[0]); (void)close(from_child[1]);
            free(sound);
            return false;
        }
    }
    sound->classifier = fork();
    if (sound->classifier < 0) {
        (void)close(to_child[0]); (void)close(to_child[1]);
        (void)close(from_child[0]); (void)close(from_child[1]);
        free(sound);
        return false;
    }
    if (sound->classifier == 0) {
        const char *const *argv = chosen;
        char *child_argv[KNVR_SOUND_ARGV_MAX + 1];
        size_t count = 0u;

        (void)dup2(to_child[0], STDIN_FILENO);
        (void)dup2(from_child[1], STDOUT_FILENO);
        (void)close(to_child[0]); (void)close(to_child[1]);
        (void)close(from_child[0]); (void)close(from_child[1]);
        while (argv[count] != NULL && count < KNVR_SOUND_ARGV_MAX) {
            child_argv[count] = (char *)argv[count];
            count++;
        }
        child_argv[count] = NULL;
        (void)execvp(child_argv[0], child_argv);
        _exit(127);
    }
    (void)close(to_child[0]);
    (void)close(from_child[1]);
    sound->to_child = to_child[1];
    sound->from_child = from_child[0];

    sound->ffmpeg = spawn_ffmpeg(url, options->log_path, &sound->audio_fd);
    if (sound->ffmpeg < 0) {
        knvr_sound_stop(sound);
        return false;
    }
    (void)signal(SIGPIPE, SIG_IGN);
    *out = sound;
    return true;
}

void knvr_sound_stop(knvr_sound *sound)
{
    int status;

    if (sound == NULL) {
        return;
    }
    if (sound->audio_fd >= 0) { (void)close(sound->audio_fd); }
    if (sound->to_child >= 0) { (void)close(sound->to_child); }
    if (sound->from_child >= 0) { (void)close(sound->from_child); }
    if (sound->ffmpeg > 0) {
        (void)kill(sound->ffmpeg, SIGTERM);
        (void)waitpid(sound->ffmpeg, &status, 0);
    }
    if (sound->classifier > 0) {
        (void)kill(sound->classifier, SIGTERM);
        (void)waitpid(sound->classifier, &status, 0);
    }
    free(sound);
}

static bool read_exactly(int fd, uint8_t *bytes, size_t size, int timeout_ms)
{
    size_t offset = 0u;

    while (offset < size) {
        struct pollfd descriptor = {fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1u, timeout_ms);
        ssize_t got;

        if (ready <= 0) {
            return false;
        }
        got = read(fd, bytes + offset, size - offset);
        if (got > 0) {
            offset += (size_t)got;
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool knvr_sound_step(
    knvr_sound *sound, knvr_sound_event *events, size_t capacity,
    size_t *count)
{
    uint8_t reply[KNVR_DETECT_BYTES];
    float rows[KNVR_DETECT_ROWS][KNVR_DETECT_COLUMNS];
    size_t written = 0u;

    if (count != NULL) {
        *count = 0u;
    }
    if (sound == NULL || sound->broken) {
        return false;
    }
    /* Fill a whole window before asking.  A partial window would make the
     * pipe self-framing only by accident. */
    {
        uint8_t *raw = (uint8_t *)sound->window;
        const size_t want = sizeof(sound->window);
        struct pollfd descriptor = {sound->audio_fd, POLLIN, 0};

        while (sound->filled < want) {
            ssize_t got;

            if (poll(&descriptor, 1u, 0) <= 0) {
                return false;   /* not a second of audio yet */
            }
            got = read(sound->audio_fd, raw + sound->filled,
                       want - sound->filled);
            if (got > 0) {
                sound->filled += (size_t)got;
                continue;
            }
            if (got < 0 && errno == EINTR) {
                continue;
            }
            return fail(sound, "the audio stream ended");
        }
        sound->filled = 0u;
        {
            size_t offset = 0u;

            while (offset < want) {
                const ssize_t put = write(sound->to_child, raw + offset,
                                          want - offset);

                if (put > 0) {
                    offset += (size_t)put;
                    continue;
                }
                if (put < 0 && errno == EINTR) {
                    continue;
                }
                return fail(sound, "the listener stopped reading");
            }
        }
    }
    if (!read_exactly(sound->from_child, reply, sizeof(reply), 5000)) {
        return fail(sound, "the listener did not answer in time");
    }
    (void)memcpy(rows, reply, sizeof(rows));
    for (size_t i = 0u; i < KNVR_DETECT_ROWS && written < capacity; i++) {
        const int class_id = (int)rows[i][0];
        const float score = rows[i][1];

        if (score < sound->min_score) {
            break;
        }
        if (knvr_sound_label(class_id) == NULL) {
            continue;
        }
        events[written].class_id = class_id;
        events[written].score = score;
        written++;
    }
    if (count != NULL) {
        *count = written;
    }
    return true;
}
