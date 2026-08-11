/*
 * Every camera at once, until told to stop.
 *
 * One pipeline per camera, all in one process and all in one loop: a
 * camera that is quiet costs a borrow that returns nothing, and a camera
 * that is busy costs what it always cost.  Nothing here is threaded,
 * because the expensive things - decoding and inference - already are
 * their own processes, and the arithmetic between them is not what makes
 * a recorder slow.
 *
 * `watch` is this with one camera and the commentary turned on.  Two
 * recorders that drift apart is the failure this family is arranged to
 * avoid, so there is one.
 */

#include "knvr_run.h"

#include "knvr_detect.h"
#include "knvr_paths.h"
#include "knvr_store.h"
#include "knvr_track.h"
#include "knvr_watch.h"
#include "knvr_zone.h"

#include "kilix_object_detect.h"
#include "kilix_rtsp.h"
#include "kilix_sound_detect.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define QUIET_SECONDS 5
#define CROP_SIZE 320
#define RING_READERS 4

/*
 * Set by a signal and read by the loop.
 *
 * The only thing a handler may safely touch, and the reason the loop
 * checks it rather than the handler doing the stopping: half a shutdown
 * from inside a signal is how a store ends up with an event that never
 * closed.
 */
static volatile sig_atomic_t stop_requested;

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

typedef struct camera_run {
    char name[KNVR_NAME_MAX];
    knvr_camera policy;
    knvr_watch *watch;
    knvr_watch_options options;
    /*
     * Whether this camera's crops go to the detector, which is shared.
     * Measured, and the reason this is not a per-camera detector: three
     * cameras each opening their own meant three model processes, six
     * with the sound classifiers, and on an eight-core machine with
     * 7 GB free every one of them died before it answered.  A detector
     * costs its model once; giving each camera its own multiplies the
     * one part of the pipeline that is genuinely expensive.
     */
    bool detects;
    /*
     * The frame whose crops are with the model, and when it was taken.
     *
     * A copy, because the answer comes back several frames later and the
     * still saved for the event has to be the picture the detection was
     * actually made in - not whatever the camera has produced since.
     */
    uint8_t *offered;
    int64_t offered_at;
    ksd_listener *sound;
    knvr_tracker *tracker;
    knvr_zones *zones;
    krtsp_frame *ring;

    int64_t event_id;
    /* Which tracks were alive at the last update, so the ones that are no
     * longer can be released from the zone table.  Its slots are finite,
     * and a recorder that never frees one runs for a week and then loses
     * inertia entirely - with no symptom except zones that fire on the
     * first frame. */
    int64_t live[KNVR_TRACK_MAX];
    size_t live_count;
    time_t last_motion;
    uint64_t suppressed;
    int64_t second;
    float motion_peak;
    float sound_peak;
    bool still_saved;
    bool rendered;

    /* Held for the lifetime of the watch: knvr_watch_options keeps the
     * pointers rather than copying them. */
    char url[KRTSP_URL_MAX];
    char record_url[KRTSP_URL_MAX];
    char record_dir[KNVR_PATH_MAX];
    char mask_path[KNVR_PATH_MAX];
    char zones_path[KNVR_PATH_MAX];
    char log_path[KNVR_PATH_MAX];
    char sound_log[KNVR_PATH_MAX];
    char classify_log[KNVR_PATH_MAX];
} camera_run;

void knvr_run_options_init(knvr_run_options *options)
{
    if (options == NULL) {
        return;
    }
    (void)memset(options, 0, sizeof(*options));
    options->publish = true;
}

bool knvr_run_ring_name(const char *camera, char *out, size_t size)
{
    if (camera == NULL || out == NULL) {
        return false;
    }
    /* Composed in one place so a viewer and a recorder cannot disagree
     * about what a camera's ring is called. */
    return snprintf(out, size, "kilix-nvr.%s", camera) > 0;
}

/* --------------------------- starting a camera --------------------------- */

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
        return false;
    }
    for (size_t i = 0u; i < krtsp_config_camera_count(config); i++) {
        const krtsp_camera *camera = krtsp_config_camera_at(config, i);
        const char *url;

        if (camera == NULL || strcmp(camera->name, name) != 0) {
            continue;
        }
        url = prefer_sub && camera->url_sub[0] != '\0' ? camera->url_sub
                                                       : camera->url_main;
        if (url[0] != '\0' && strlen(url) < size) {
            (void)snprintf(out, size, "%s", url);
            found = true;
        }
        break;
    }
    krtsp_config_free(config);
    return found;
}

static void camera_stop(camera_run *run)
{
    knvr_watch_stop(run->watch);
    free(run->offered);
    ksd_close(run->sound);
    knvr_tracker_free(run->tracker);
    knvr_zones_free(run->zones);
    /* The producer owns the object, so it unlinks it: a ring left behind
     * by a dead recorder is a viewer attaching to frames that stopped. */
    krtsp_frame_free(run->ring);
    (void)memset(run, 0, sizeof(*run));
}

static bool camera_start(camera_run *run, const knvr_camera *policy,
                         const knvr_run_options *options)
{
    (void)memset(run, 0, sizeof(*run));
    run->policy = *policy;
    (void)snprintf(run->name, sizeof(run->name), "%.*s",
                   (int)sizeof(run->name) - 1, policy->name);

    if (!resolve_url(run->name, true, run->url, sizeof(run->url))) {
        (void)fprintf(stderr, "kilix-nvr: %s: no stream URL in cameras.conf\n",
                      run->name);
        return false;
    }
    knvr_watch_options_init(&run->options);
    /* ffmpeg's stderr goes to a file, never the terminal: one warning in
     * the alternate screen corrupts a display, and a flaky camera
     * produces plenty. */
    if (knvr_paths_state_file(run->log_path, sizeof(run->log_path),
                              "ffmpeg.log")) {
        run->options.log_path = run->log_path;
    }
    if (policy->record == KNVR_RECORD_CONTINUOUS) {
        char segments[KNVR_PATH_MAX];

        /* The main stream for the archive, the substream for motion:
         * full-quality footage without decoding it. */
        if (resolve_url(run->name, false, run->record_url,
                        sizeof(run->record_url))) {
            run->options.record_url = run->record_url;
        }
        if (knvr_paths_subdir(segments, sizeof(segments), "segments") &&
            snprintf(run->record_dir, sizeof(run->record_dir), "%s/%s",
                     segments, run->name) > 0) {
            (void)mkdir(run->record_dir, 0700);
            run->options.record_dir = run->record_dir;
            /* Matroska, measured: pcm_alaw survives -c copy untouched,
             * which mp4 cannot promise. */
            run->options.segment_seconds = 60;
            run->options.record_audio = policy->audio;
        }
    }
    if (policy->mask[0] != '\0') {
        char masks[KNVR_PATH_MAX];

        if (knvr_paths_subdir(masks, sizeof(masks), "masks") &&
            snprintf(run->mask_path, sizeof(run->mask_path), "%s/%s", masks,
                     policy->mask) > 0) {
            run->options.mask_path = run->mask_path;
        }
    }
    if (!knvr_watch_start(&run->watch, run->url, &run->options)) {
        (void)fprintf(stderr, "kilix-nvr: %s: %s\n", run->name,
                      knvr_watch_error(run->watch) != NULL
                          ? knvr_watch_error(run->watch)
                          : "could not start the camera");
        knvr_watch_stop(run->watch);
        run->watch = NULL;
        return false;
    }
    run->offered = malloc((size_t)knvr_watch_width(run->watch) *
                          (size_t)knvr_watch_height(run->watch) * 4u);
    if (run->offered == NULL) {
        knvr_watch_stop(run->watch);
        run->watch = NULL;
        return false;
    }
    {
        knvr_tracker_options track_options;

        knvr_tracker_options_init(&track_options);
        if (!knvr_tracker_create(&run->tracker, &track_options)) {
            (void)fprintf(stderr,
                          "kilix-nvr: %s: no tracker; detections only\n",
                          run->name);
        }
    }
    /* Loaded after the camera, because the map has to be reconciled
     * against the geometry the boxes will actually arrive in. */
    if (policy->zones[0] != '\0') {
        char directory[KNVR_PATH_MAX];

        if (knvr_paths_subdir(directory, sizeof(directory), "zones") &&
            snprintf(run->zones_path, sizeof(run->zones_path), "%s/%s",
                     directory, policy->zones) > 0 &&
            !knvr_zones_load(&run->zones, run->zones_path,
                             knvr_watch_width(run->watch),
                             knvr_watch_height(run->watch))) {
            (void)fprintf(stderr,
                          "kilix-nvr: %s: cannot read the zone map; no "
                          "zones\n", run->name);
        }
    }
    if (policy->sound_events) {
        ksd_options sound_options;
        char sound_url[KRTSP_URL_MAX];

        ksd_options_init(&sound_options);
        /* Its own log.  Two ffmpegs sharing one file makes "which of them
         * is complaining" a guess. */
        if (knvr_paths_state_file(run->sound_log, sizeof(run->sound_log),
                                  "ffmpeg-audio.log")) {
            sound_options.log_path = run->sound_log;
        }
        if (knvr_paths_state_file(run->classify_log,
                                  sizeof(run->classify_log),
                                  "classify.log")) {
            sound_options.classifier_log_path = run->classify_log;
        }
        /* The main stream, which is where the audio is: substreams
         * frequently carry none at all. */
        if (resolve_url(run->name, false, sound_url, sizeof(sound_url)) &&
            !ksd_open(&run->sound, sound_url, &sound_options)) {
            (void)fprintf(stderr, "kilix-nvr: %s: no listener; sight only\n",
                          run->name);
        }
    }
    /* `always` runs it.  `on-view` is blind with no viewer attached, and
     * `cameras` is the one place that says so.  The detector itself is
     * opened once for the whole run, not here. */
    run->detects = policy->detect == KNVR_DETECT_ALWAYS;
    if (options->publish) {
        char ring[KNVR_NAME_MAX + 32];

        if (knvr_run_ring_name(run->name, ring, sizeof(ring)) &&
            !krtsp_frame_init_shared(&run->ring, ring,
                                     knvr_watch_width(run->watch),
                                     knvr_watch_height(run->watch),
                                     RING_READERS)) {
            /* Said once and carried on: a recorder that refuses to record
             * because nothing can watch it has the priority backwards. */
            (void)fprintf(stderr,
                          "kilix-nvr: %s: cannot publish frames; a viewer "
                          "will open its own\n", run->name);
            run->ring = NULL;
        }
    }
    return true;
}

/* ------------------------------- one frame ------------------------------- */

/* BGRA to a binary PPM.  The one format both feh and the review browser
 * open without a decoder, which is what a still saved at three in the
 * morning has to be. */
static bool write_ppm(const char *path, const uint8_t *bgra, int width,
                      int height)
{
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        return false;
    }
    (void)fprintf(file, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        (void)fputc(bgra[i * 4 + 2], file);
        (void)fputc(bgra[i * 4 + 1], file);
        (void)fputc(bgra[i * 4 + 0], file);
    }
    return fclose(file) == 0;
}

static void write_pulse(camera_run *run, knvr_store *store, int64_t now,
                        float motion, float audio)
{
    if (motion > run->motion_peak) { run->motion_peak = motion; }
    if (audio > run->sound_peak) { run->sound_peak = audio; }
    if (run->second == 0) {
        run->second = now;
        return;
    }
    if (now == run->second) {
        return;
    }
    (void)knvr_store_pulse(store, run->name, run->second, run->motion_peak,
                           run->sound_peak);
    run->motion_peak = 0.0f;
    run->sound_peak = 0.0f;
    run->second = now;
}

static void save_still(knvr_store *store, camera_run *run,
                       const uint8_t *frame)
{
    char directory[KNVR_PATH_MAX];
    char path[KNVR_PATH_MAX];

    /* One per event, not one per detection: twenty stills of the same
     * person crossing the same yard is a directory nobody reads. */
    if (run->still_saved || run->event_id == 0) {
        return;
    }
    if (!knvr_paths_subdir(directory, sizeof(directory), "media")) {
        return;
    }
    if (snprintf(path, sizeof(path), "%s/%s-%lld.ppm", directory, run->name,
                 (long long)run->event_id) < 0) {
        return;
    }
    if (write_ppm(path, frame, knvr_watch_width(run->watch),
                  knvr_watch_height(run->watch))) {
        (void)knvr_store_add_media(store, run->event_id, "still", path);
        run->still_saved = true;
    }
}

static int64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return (int64_t)time(NULL) * 1000;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/*
 * Ask, without waiting for the answer.
 *
 * The crops go out and the loop carries on decoding; the reply is picked
 * up in some later iteration by take_detections().  Measured before this
 * split: inference in the frame loop dropped three cameras from 30 frames
 * a second to under 5 - including the cameras nothing was moving in,
 * because they were all queued behind one model.
 */
static bool offer_detections(camera_run *run, kod_detector *detector,
                             const uint8_t *frame, const knvr_box *boxes,
                             size_t count)
{
    kod_rect crops[KOD_REGION_MAX];
    kod_rect moved[KNVR_MOTION_BOX_MAX];
    size_t crop_count;

    if (!run->detects || detector == NULL || run->offered == NULL) {
        return false;
    }
    /* Crops around what moved, and the boxes handed in here have already
     * been through the preclusive-zone filter - so an ignored zone costs
     * no inference at all. */
    for (size_t i = 0u; i < count && i < KNVR_MOTION_BOX_MAX; i++) {
        moved[i].x = boxes[i].x;
        moved[i].y = boxes[i].y;
        moved[i].w = boxes[i].w;
        moved[i].h = boxes[i].h;
    }
    crop_count = kod_regions(moved, count, knvr_watch_width(run->watch),
                             knvr_watch_height(run->watch), CROP_SIZE, crops,
                             KOD_REGION_MAX, NULL);
    if (crop_count == 0u) {
        return false;
    }
    if (!kod_offer(detector, frame, knvr_watch_width(run->watch),
                   knvr_watch_height(run->watch), crops, crop_count)) {
        return false;
    }
    /* Kept because the still belongs to the frame the model was shown,
     * not to whatever has arrived by the time it answers. */
    (void)memcpy(run->offered, frame,
                 (size_t)knvr_watch_width(run->watch) *
                     (size_t)knvr_watch_height(run->watch) * 4u);
    run->offered_at = (int64_t)time(NULL);
    return true;
}

static void apply_detections(camera_run *run, knvr_store *store,
                             const kod_box *seen, size_t detections,
                             int64_t at, bool verbose)
{
    char zone_of[KNVR_TRACK_MAX][KNVR_STORE_LABEL_MAX];
    int64_t zone_track[KNVR_TRACK_MAX];
    size_t zone_count = 0u;

    {
        knvr_detection_box found[KOD_BOX_MAX];

        for (size_t i = 0u; i < detections; i++) {
            found[i] = knvr_detect_from(&seen[i]);
        }
        (void)knvr_tracker_update(run->tracker, found, detections,
                                  monotonic_ms());
    }
    /* Tracks age out inside update() and nowhere else, so this is the one
     * moment at which "gone" is knowable. */
    if (run->zones != NULL) {
        int64_t still_here[KNVR_TRACK_MAX];
        size_t here = 0u;

        for (size_t t = 0u; t < KNVR_TRACK_MAX; t++) {
            const knvr_track *object = knvr_tracker_at(run->tracker, t);

            if (object == NULL) {
                break;
            }
            still_here[here++] = object->id;
        }
        for (size_t was = 0u; was < run->live_count; was++) {
            bool found = false;

            for (size_t is = 0u; is < here && !found; is++) {
                found = still_here[is] == run->live[was];
            }
            if (!found) {
                knvr_zones_forget(run->zones, run->live[was]);
            }
        }
        (void)memcpy(run->live, still_here, here * sizeof(*still_here));
        run->live_count = here;
    }
    /* Zones per track rather than per detection: inertia belongs to the
     * object - "has been in the drive for three frames" is a fact about
     * the car, not about the frame. */
    for (size_t t = 0u; t < KNVR_TRACK_MAX; t++) {
        const knvr_track *object = knvr_tracker_at(run->tracker, t);
        knvr_object row;
        knvr_zone_hit hit;

        if (object == NULL) {
            break;
        }
        (void)memset(&hit, 0, sizeof(hit));
        if (run->zones != NULL) {
            (void)knvr_zones_track(run->zones, object->id, &object->box,
                                   monotonic_ms(), &hit);
        }
        zone_track[zone_count] = object->id;
        zone_of[zone_count][0] = '\0';
        if (hit.settled) {
            const char *zone_name = knvr_zone_name(run->zones, hit.region);

            if (zone_name != NULL) {
                (void)snprintf(zone_of[zone_count], KNVR_STORE_LABEL_MAX,
                               "%s", zone_name);
            }
        }
        zone_count++;
        if (!object->confirmed) {
            continue;
        }
        (void)memset(&row, 0, sizeof(row));
        row.event = run->event_id;
        row.track = object->id;
        (void)snprintf(row.camera, sizeof(row.camera), "%s", run->name);
        (void)snprintf(row.label, sizeof(row.label), "%s",
                       kod_label(object->class_id));
        row.score = (double)object->score;
        row.first_seen = at;
        row.last_seen = at;
        row.travelled = object->travelled;
        row.stationary = object->stationary;
        (void)snprintf(row.zone, sizeof(row.zone), "%.*s",
                       KNVR_STORE_LABEL_MAX - 1, zone_of[zone_count - 1u]);
        (void)knvr_store_put_object(store, &row);
    }
    for (size_t d = 0u; d < detections; d++) {
        knvr_detection record;
        const knvr_detection_box box = knvr_detect_from(&seen[d]);

        (void)memset(&record, 0, sizeof(record));
        record.event = run->event_id;
        record.at = at;
        (void)snprintf(record.label, sizeof(record.label), "%s",
                       knvr_detect_label(box.class_id));
        record.score = (double)box.score;
        record.x = box.x;
        record.y = box.y;
        record.w = box.w;
        record.h = box.h;
        record.track = knvr_tracker_assigned(run->tracker, d);
        for (size_t z = 0u; z < zone_count; z++) {
            if (zone_track[z] == record.track) {
                (void)snprintf(record.zone, sizeof(record.zone), "%.*s",
                               KNVR_STORE_LABEL_MAX - 1, zone_of[z]);
                break;
            }
        }
        (void)knvr_store_add_detection(store, &record);
        if (verbose) {
            (void)printf("  %s: %s %.2f%s%s (track %lld)\n", run->name,
                         record.label, record.score,
                         record.zone[0] != '\0' ? " in " : "", record.zone,
                         (long long)record.track);
        }
    }
    if (detections > 0u && run->policy.record != KNVR_RECORD_OFF) {
        save_still(store, run, run->offered);
    }
}

static void listen(camera_run *run, knvr_store *store, bool verbose)
{
    ksd_event heard[8];
    size_t count = 0u;

    if (run->sound == NULL) {
        return;
    }
    if (!ksd_step(run->sound, heard, 8u, &count)) {
        if (ksd_error(run->sound) != NULL) {
            /* Once, then sight-only.  A camera with no audio reaches here
             * immediately, and repeating it every second would bury
             * everything else. */
            (void)fprintf(stderr, "kilix-nvr: %s: %s; sight only\n",
                          run->name, ksd_error(run->sound));
            ksd_close(run->sound);
            run->sound = NULL;
        }
        return;
    }
    if (ksd_level(run->sound) > run->sound_peak) {
        run->sound_peak = ksd_level(run->sound);
    }
    for (size_t h = 0u; h < count; h++) {
        knvr_detection record;
        const int64_t at = (int64_t)time(NULL);

        /* A sound with nothing moving still deserves an event: a noise in
         * the dark is exactly what a motion gate cannot see. */
        if (run->event_id == 0 &&
            knvr_store_event_open(store, run->name, KNVR_TRIGGER_SOUND, at,
                                  &run->event_id)) {
            run->last_motion = (time_t)at;
            run->still_saved = false;
        }
        (void)memset(&record, 0, sizeof(record));
        record.event = run->event_id;
        record.at = at;
        (void)snprintf(record.label, sizeof(record.label), "%s",
                       ksd_label(heard[h].class_id));
        record.score = (double)heard[h].score;
        (void)knvr_store_add_detection(store, &record);
        if (verbose) {
            (void)printf("  %s: heard %s %.2f\n", run->name, record.label,
                         record.score);
        }
    }
}

static void camera_step(camera_run *run, kod_detector *detector,
                        knvr_store *store, const knvr_run_options *options,
                        bool *offered)
{
    knvr_box boxes[KNVR_MOTION_BOX_MAX];
    knvr_watch_stats stats;
    const uint8_t *frame = NULL;
    size_t count = 0u;
    const int64_t now = (int64_t)time(NULL);

    listen(run, store, options->verbose);
    if (run->watch == NULL) {
        return;
    }
    if (!knvr_watch_step(run->watch, &frame, boxes, KNVR_MOTION_BOX_MAX,
                         &count)) {
        /* No new frame.  The second still has to turn over, or a quiet
         * camera's pulse stops rather than reading zero. */
        write_pulse(run, store, now, 0.0f, 0.0f);
        return;
    }
    knvr_watch_get_stats(run->watch, &stats);
    write_pulse(run, store, now, stats.motion_fraction, 0.0f);

    if (run->ring != NULL) {
        /* Published before anything is drawn on it: a viewer wants the
         * camera's frame, and what a recorder thinks about it arrives
         * through the store. */
        uint8_t *back = krtsp_frame_back(run->ring);
        const size_t size = krtsp_frame_size(run->ring);
        bool dropped = false;

        if (back != NULL && size > 0u) {
            (void)memcpy(back, frame, size);
            krtsp_frame_publish(run->ring, &dropped);
        }
    }

    /*
     * Preclusive zones act at the gate, which is where ZoneMinder puts
     * them: movement in a zone marked preclusive is not evidence, so it
     * must not open an event and must not wake the detector.
     */
    if (run->zones != NULL && count > 0u) {
        size_t kept = 0u;

        for (size_t b = 0u; b < count; b++) {
            const uint8_t region = knvr_zones_at_point(
                run->zones, boxes[b].x + boxes[b].w / 2,
                boxes[b].y + boxes[b].h - 1);

            if (knvr_zone_is_preclusive(run->zones, region)) {
                run->suppressed++;
                continue;
            }
            boxes[kept++] = boxes[b];
        }
        count = kept;
    }
    if (count > 0u) {
        if (run->event_id == 0) {
            if (knvr_store_event_open(store, run->name, KNVR_TRIGGER_MOTION,
                                      now, &run->event_id)) {
                run->still_saved = false;
                if (options->verbose) {
                    (void)printf("  %s: event %lld opened\n", run->name,
                                 (long long)run->event_id);
                }
            }
        } else {
            (void)knvr_store_event_touch(store, run->event_id);
        }
        run->last_motion = (time_t)now;
        if (options->verbose) {
            (void)printf("  %s: motion, %zu region%s\n", run->name, count,
                         count == 1u ? "" : "s");
        }
        /*
         * At most one camera's crops are with the model at a time, and
         * whoever gets there first this iteration takes it.  Offering is
         * refused rather than queued: a queue of crops is a queue of
         * stale pictures, and a detection reported thirty seconds after
         * the car left is worse than no detection.
         */
        if (offered != NULL && !*offered) {
            *offered = offer_detections(run, detector, frame, boxes, count);
        }
        if (options->render != NULL && !run->rendered) {
            const int width = knvr_watch_width(run->watch);
            const int height = knvr_watch_height(run->watch);
            uint8_t *copy = malloc((size_t)width * (size_t)height * 4u);

            if (copy != NULL) {
                (void)memcpy(copy, frame,
                             (size_t)width * (size_t)height * 4u);
                knvr_watch_draw_boxes(copy, width, height, boxes, count);
                knvr_track_draw(copy, width, height, run->tracker);
                {
                    FILE *file = fopen(options->render, "wb");

                    if (file != NULL) {
                        (void)fprintf(file, "P6\n%d %d\n255\n", width,
                                      height);
                        for (int i = 0; i < width * height; i++) {
                            (void)fputc(copy[i * 4 + 2], file);
                            (void)fputc(copy[i * 4 + 1], file);
                            (void)fputc(copy[i * 4 + 0], file);
                        }
                        if (fclose(file) == 0) {
                            run->rendered = true;
                            (void)printf("  wrote %s\n", options->render);
                        }
                    }
                }
                free(copy);
            }
        }
    } else if (run->event_id != 0 &&
               now - (int64_t)run->last_motion >= QUIET_SECONDS) {
        /* Quiet for long enough to call it over.  Closing on the next
         * still frame would split one person walking past into a dozen
         * events. */
        (void)knvr_store_event_close(store, run->event_id, now);
        if (options->verbose) {
            (void)printf("  %s: event %lld closed\n", run->name,
                         (long long)run->event_id);
        }
        run->event_id = 0;
    }
}

/* --------------------------------- running -------------------------------- */

int knvr_run(knvr_config *config, const knvr_run_options *options)
{
    knvr_run_options defaults;
    knvr_camera cameras[KNVR_CAMERA_MAX];
    camera_run *runs;
    knvr_store *store = NULL;
    kod_detector *detector = NULL;
    /* The camera whose crops are with the model, and where the next
     * offer starts looking. */
    camera_run *waiting = NULL;
    size_t turn = 0u;
    size_t configured = 0u;
    size_t count = 0u;
    time_t deadline;
    struct sigaction action;

    if (config == NULL) {
        return 1;
    }
    if (options == NULL) {
        knvr_run_options_init(&defaults);
        options = &defaults;
    }
    if (!knvr_config_list(config, cameras, KNVR_CAMERA_MAX, &configured)) {
        (void)fprintf(stderr, "kilix-nvr: cannot list cameras\n");
        return 1;
    }
    if (configured == 0u) {
        (void)fprintf(stderr,
                      "kilix-nvr: no cameras configured; add one with "
                      "`kilix-nvr add <name>`\n");
        return 1;
    }
    if (!knvr_store_open(&store, NULL)) {
        (void)fprintf(stderr, "kilix-nvr: cannot open the event store\n");
        return 1;
    }
    runs = calloc(KNVR_CAMERA_MAX, sizeof(*runs));
    if (runs == NULL) {
        knvr_store_close(store);
        return 1;
    }

    for (size_t i = 0u; i < configured && i < KNVR_CAMERA_MAX; i++) {
        bool wanted = options->cameras == NULL;

        for (size_t w = 0u; w < options->count && !wanted; w++) {
            wanted = strcmp(options->cameras[w], cameras[i].name) == 0;
        }
        if (!wanted) {
            continue;
        }
        /* Anything this camera left open belongs to a previous run: an
         * event with no end time never appears in a query bounded by when
         * it finished, so a crash must not leave one behind. */
        (void)knvr_store_close_stale(store, cameras[i].name,
                                     (int64_t)time(NULL));
        if (camera_start(&runs[count], &cameras[i], options)) {
            (void)printf("%s at %dx%d%s%s%s%s\n", runs[count].name,
                         knvr_watch_width(runs[count].watch),
                         knvr_watch_height(runs[count].watch),
                         runs[count].options.mask_path != NULL ? " masked" : "",
                         runs[count].zones != NULL ? " zoned" : "",
                         runs[count].detects ? " detecting" : "",
                         runs[count].ring != NULL ? " published" : "");
            count++;
        }
    }
    if (count == 0u) {
        (void)fprintf(stderr, "kilix-nvr: no camera could be started\n");
        free(runs);
        knvr_store_close(store);
        return 1;
    }
    /*
     * One detector for every camera, opened only if some camera wants it.
     *
     * Cameras take turns rather than run at once, which is the honest
     * arrangement: inference is serial on this hardware whatever the
     * caller pretends, and queueing for one process is how that shows up
     * as latency instead of as six processes fighting over eight cores.
     */
    for (size_t i = 0u; i < count; i++) {
        kod_options detector_options;
        char detect_log[KNVR_PATH_MAX];

        if (!runs[i].detects) {
            continue;
        }
        kod_options_init(&detector_options);
        detector_options.size = CROP_SIZE;
        if (knvr_paths_state_file(detect_log, sizeof(detect_log),
                                  "detect.log")) {
            detector_options.log_path = detect_log;
        }
        if (!kod_open(&detector, &detector_options)) {
            /* A degradation, not a fault: motion-only still records. */
            (void)fprintf(stderr, "kilix-nvr: no detector; motion only\n");
            detector = NULL;
        }
        break;
    }

    /* Signals rather than a keypress: this is meant to be a service, and
     * a service is stopped by its supervisor. */
    stop_requested = 0;
    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);

    deadline = options->seconds > 0 ? time(NULL) + options->seconds : 0;
    while (!stop_requested) {
        struct timespec pause = {0, 20 * 1000 * 1000};
        bool offered = detector == NULL || kod_busy(detector);

        if (deadline > 0 && time(NULL) >= deadline) {
            break;
        }
        /*
         * The answer to the last question, if it has arrived.  Collected
         * before the cameras are stepped so the model is free again as
         * early as possible, and always against the camera that asked -
         * `waiting` is why the detector may be shared at all.
         */
        if (detector != NULL && kod_busy(detector) && waiting != NULL) {
            kod_box seen[KOD_BOX_MAX];
            size_t detections = 0u;
            bool done = false;

            if (!kod_take(detector, seen, KOD_BOX_MAX, &detections, &done)) {
                /* The detector is shared, so losing it costs every camera.
                 * Reported against the one that was using it at the time,
                 * because that is the only camera whose crops are known. */
                (void)fprintf(stderr,
                              "kilix-nvr: %s: %s; motion only from here\n",
                              waiting->name,
                              kod_error(detector) != NULL
                                  ? kod_error(detector)
                                  : "the detector failed");
                kod_close(detector);
                detector = NULL;
                waiting = NULL;
            } else if (done) {
                apply_detections(waiting, store, seen, detections,
                                 waiting->offered_at, options->verbose);
                waiting = NULL;
            }
        }
        /*
         * Started from a different camera each time.  Without this the
         * first camera in the list takes the detector on every iteration
         * it has motion, and a busy driveway starves a quiet doorway of
         * the one thing that would say a person was at it.
         */
        for (size_t n = 0u; n < count; n++) {
            const size_t i = (turn + n) % count;
            bool took = offered;

            camera_step(&runs[i], detector, store, options, &took);
            if (took && !offered) {
                offered = true;
                waiting = &runs[i];
                turn = (i + 1u) % count;
            }
        }
        (void)nanosleep(&pause, NULL);
    }
    /* A batch still in flight at shutdown is abandoned: its camera is
     * about to be torn down, and there is nothing left to attribute the
     * answer to. */

    for (size_t i = 0u; i < count; i++) {
        knvr_watch_stats stats;

        if (runs[i].event_id != 0) {
            (void)knvr_store_event_close(store, runs[i].event_id,
                                         (int64_t)time(NULL));
        }
        knvr_watch_get_stats(runs[i].watch, &stats);
        (void)printf("%s: %llu frames, %llu with motion, %lld object%s%s",
                     runs[i].name, (unsigned long long)stats.frames,
                     (unsigned long long)stats.motion_frames,
                     (long long)knvr_tracker_total(runs[i].tracker),
                     knvr_tracker_total(runs[i].tracker) == 1 ? "" : "s",
                     runs[i].suppressed > 0u ? "" : "\n");
        if (runs[i].suppressed > 0u) {
            (void)printf(", %llu suppressed by preclusive zones\n",
                         (unsigned long long)runs[i].suppressed);
        }
        camera_stop(&runs[i]);
    }
    if (detector != NULL) {
        /* Shared, so it is reported once and not per camera.  This is the
         * number that separates "the gate never woke it" from "it looked
         * and there was nothing there" - two very different faults that
         * produce the same empty event list. */
        (void)printf("detector: %llu crops\n",
                     (unsigned long long)kod_crops(detector));
    }
    kod_close(detector);
    free(runs);
    knvr_store_close(store);
    return 0;
}
