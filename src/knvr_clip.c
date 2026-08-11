/*
 * Events cut out of the segments they span, and the segments swept after.
 */

/* strptime is XSI, and the build asks only for POSIX.  Declared here
 * rather than widening the whole module's feature set. */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "knvr_clip.h"

#include "knvr_paths.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * How long a `clips` camera keeps the segments it cuts from.
 *
 * Long enough to cover a cut that failed and a machine that was asleep
 * when the event closed; short enough that "clips" does not quietly mean
 * "continuous plus clips".  An hour of substream segments is tens of
 * megabytes; a day of them is the thing this mode exists to avoid.
 */
#define CLIP_SEGMENT_KEEP_SECONDS (2 * 60 * 60)

/* Pre-roll, in seconds.  The ten seconds before somebody enters frame is
 * the part worth having, and the only way to have it is to have been
 * recording already. */
#define PREROLL_SECONDS 10

/*
 * When a segment starts, from its name.
 *
 * The name is the answer and the mtime is not: ffmpeg's -strftime writes
 * the file when it OPENS it, so the name is the start and the mtime is
 * the close, a whole segment later.  Picking by mtime - which this did -
 * selects the segment after the one containing the event about as often
 * as not, and makes the pre-roll offset negative, which clamps to zero
 * and quietly turns "ten seconds before" into "wherever this file began".
 */
static bool segment_started(const char *name, time_t *out)
{
    struct tm parts;
    const char *rest;

    (void)memset(&parts, 0, sizeof(parts));
    rest = strptime(name, "%Y-%m-%d_%H.%M.%S", &parts);
    if (rest == NULL) {
        return false;
    }
    /* Written in local time by strftime, so read back the same way. */
    parts.tm_isdst = -1;
    *out = mktime(&parts);
    return *out != (time_t)-1;
}

bool knvr_clip_segment_covering(const char *camera, int64_t at, char *out,
                                size_t size)
{
    char directory[KNVR_PATH_MAX];
    char segments[KNVR_PATH_MAX];
    DIR *handle;
    struct dirent *entry;
    time_t best = 0;
    bool found = false;

    if (camera == NULL || out == NULL) {
        return false;
    }
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
        time_t started = 0;

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
        if (!segment_started(entry->d_name, &started)) {
            continue;
        }
        /* The last segment that had already begun.  The one still being
         * written is a legitimate answer - an event that just closed is
         * usually inside it - and copying from a growing file is fine,
         * because -c copy reads what is there. */
        if (started <= (time_t)at && started > best) {
            best = started;
            (void)snprintf(out, size, "%s", candidate);
            found = true;
        }
    }
    (void)closedir(handle);
    return found;
}

bool knvr_clip_ready(const knvr_event *event, int64_t now, int patience)
{
    char segments[KNVR_PATH_MAX];
    char directory[KNVR_PATH_MAX];
    DIR *handle;
    struct dirent *entry;
    const int64_t ended = event != NULL && event->ended > 0 ? event->ended
                                                            : 0;
    bool rotated = false;

    if (event == NULL) {
        return false;
    }
    if (ended > 0 && now - ended >= (int64_t)patience) {
        return true;
    }
    if (!knvr_paths_subdir(segments, sizeof(segments), "segments") ||
        snprintf(directory, sizeof(directory), "%s/%s", segments,
                 event->camera) < 0) {
        return true;   /* nothing to wait for */
    }
    handle = opendir(directory);
    if (handle == NULL) {
        return true;
    }
    while ((entry = readdir(handle)) != NULL && !rotated) {
        time_t started = 0;

        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!segment_started(entry->d_name, &started)) {
            continue;
        }
        /* A segment that began after the event ended means the one
         * holding it has been closed. */
        rotated = (int64_t)started > ended;
    }
    (void)closedir(handle);
    return rotated;
}

/* Where the clip goes, and how far into the segment it starts. */
static bool plan_cut(const knvr_event *event, char *segment, size_t segment_size,
                     char *output, size_t output_size, long *offset,
                     long long *duration)
{
    char clips[KNVR_PATH_MAX];

    if (event == NULL ||
        !knvr_clip_segment_covering(event->camera, event->started, segment,
                                    segment_size)) {
        return false;
    }
    if (!knvr_paths_subdir(clips, sizeof(clips), "clips") ||
        snprintf(output, output_size, "%s/%s-%lld.mkv", clips, event->camera,
                 (long long)event->id) < 0) {
        return false;
    }
    *offset = 0;
    {
        const char *slash = strrchr(segment, '/');
        time_t started = 0;

        if (slash != NULL && segment_started(slash + 1, &started)) {
            const long into = (long)(event->started - started);

            *offset = into > PREROLL_SECONDS ? into - PREROLL_SECONDS : 0;
        }
    }
    *duration = (long long)((event->ended > event->started
                                 ? event->ended - event->started
                                 : PREROLL_SECONDS) +
                            PREROLL_SECONDS);
    return true;
}

pid_t knvr_clip_start(const knvr_event *event, char *out, size_t size)
{
    char segment[KNVR_PATH_MAX];
    char output[KNVR_PATH_MAX];
    char start[32];
    char duration[32];
    long offset = 0;
    long long seconds = 0;
    pid_t child;

    if (!plan_cut(event, segment, sizeof(segment), output, sizeof(output),
                  &offset, &seconds)) {
        return -1;
    }
    (void)snprintf(start, sizeof(start), "%ld", offset);
    (void)snprintf(duration, sizeof(duration), "%lld", seconds);
    child = fork();
    if (child < 0) {
        return -1;
    }
    if (child == 0) {
        /* -ss before -i, so ffmpeg seeks by keyframe instead of decoding
         * and discarding everything up to the offset. */
        (void)execlp("ffmpeg", "ffmpeg", "-hide_banner", "-loglevel", "error",
                     "-nostdin", "-ss", start, "-i", segment, "-t", duration,
                     "-c", "copy", "-y", output, (char *)NULL);
        _exit(127);
    }
    if (out != NULL) {
        (void)snprintf(out, size, "%s", output);
    }
    return child;
}

bool knvr_clip_finish(knvr_store *store, pid_t child, int64_t event_id,
                      const char *path, bool *ok)
{
    int status = 0;
    pid_t done;

    if (ok != NULL) {
        *ok = false;
    }
    if (child <= 0) {
        return true;
    }
    done = waitpid(child, &status, WNOHANG);
    if (done == 0) {
        return false;   /* still cutting */
    }
    if (done != child) {
        return true;    /* gone, and not ours to mourn */
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && path != NULL) {
        (void)knvr_store_add_media(store, event_id, "clip", path);
        if (ok != NULL) {
            *ok = true;
        }
    } else if (path != NULL) {
        /* A half-written clip is worse than none: it indexes as footage
         * and plays as nothing. */
        (void)remove(path);
    }
    return true;
}

bool knvr_clip_cut(knvr_store *store, const knvr_event *event, char *out,
                   size_t size)
{
    char path[KNVR_PATH_MAX];
    pid_t child = knvr_clip_start(event, path, sizeof(path));
    bool ok = false;

    if (child < 0) {
        return false;
    }
    /* The blocking form is the same child, waited for.  One
     * implementation, so a clip cut by the recorder and a clip cut by
     * hand cannot differ. */
    while (!knvr_clip_finish(store, child, event->id, path, &ok)) {
        struct timespec pause = {0, 20 * 1000 * 1000};

        (void)nanosleep(&pause, NULL);
    }
    if (ok && out != NULL) {
        (void)snprintf(out, size, "%s", path);
    }
    return ok;
}

/* ------------------------------ the segments ----------------------------- */

static bool sweep_camera(const knvr_camera *camera, int64_t now, bool dry_run,
                         knvr_prune_result *result)
{
    char segments[KNVR_PATH_MAX];
    char directory[KNVR_PATH_MAX];
    DIR *handle;
    struct dirent *entry;
    int64_t keep_seconds;

    if (camera->record == KNVR_RECORD_CLIPS) {
        keep_seconds = CLIP_SEGMENT_KEEP_SECONDS;
    } else if (camera->record == KNVR_RECORD_CONTINUOUS) {
        if (camera->retain_days <= 0) {
            /* No age rule for this camera, so the global size cap is the
             * only limit - which is a choice, and not one to override
             * here by inventing a default nobody set. */
            return true;
        }
        keep_seconds = (int64_t)camera->retain_days * 24 * 60 * 60;
    } else {
        /* Not recording video at all; anything under its directory is
         * left from a mode it is no longer in, and deleting it because
         * the setting changed is not this function's decision. */
        return true;
    }
    if (!knvr_paths_subdir(segments, sizeof(segments), "segments") ||
        snprintf(directory, sizeof(directory), "%s/%s", segments,
                 camera->name) < 0) {
        return false;
    }
    handle = opendir(directory);
    if (handle == NULL) {
        return true;   /* nothing recorded yet is not a failure */
    }
    while ((entry = readdir(handle)) != NULL) {
        char path[KNVR_PATH_MAX];
        struct stat info;

        if (entry->d_name[0] == '.') {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", directory,
                     entry->d_name) < 0) {
            continue;
        }
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
            continue;
        }
        if ((int64_t)info.st_mtime > now - keep_seconds) {
            continue;
        }
        /* The one being written has the newest mtime and is therefore
         * never old enough to reach here - but it is worth saying why
         * that is safe rather than leaving it to arithmetic. */
        if (result != NULL) {
            result->media_removed++;
            result->bytes_freed += (uint64_t)info.st_size;
        }
        if (!dry_run && remove(path) != 0) {
            (void)fprintf(stderr, "kilix-nvr: cannot remove %s\n", path);
        }
    }
    (void)closedir(handle);
    return true;
}

bool knvr_clip_prune_segments(knvr_config *config, int64_t now, bool dry_run,
                              knvr_prune_result *result)
{
    knvr_camera cameras[KNVR_CAMERA_MAX];
    size_t count = 0u;

    if (config == NULL) {
        return false;
    }
    if (!knvr_config_list(config, cameras, KNVR_CAMERA_MAX, &count)) {
        return false;
    }
    for (size_t i = 0u; i < count && i < KNVR_CAMERA_MAX; i++) {
        if (!sweep_camera(&cameras[i], now, dry_run, result)) {
            return false;
        }
    }
    return true;
}
