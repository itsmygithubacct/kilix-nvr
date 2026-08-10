/*
 * One camera: decode once, difference a small copy, report what moved.
 *
 * Two things here are decisions rather than plumbing.
 *
 * The detector's mask comes from kilix-mask's exclusion form, which yields
 * 0 where the operator painted.  kmd_detector_set_mask() treats 0 as
 * "ignore this pixel".  So the operator paints the tree, and neither side
 * inverts anything - the mistake this avoids is a camera that detects only
 * the tree, which looks like a working camera until somebody checks.
 *
 * And boxes need no rescaling.  Motion runs on a frame about a sixth of
 * the size, but kmd_detect() already reports in source coordinates "so a
 * caller never has to know detect_height" - scaling them again here was
 * the first bug this file had, and it produced boxes with negative
 * heights pointing off the bottom of the frame.
 */

#include "knvr_watch.h"

#include "kilix_mask.h"
#include "kilix_motion_detect.h"
#include "kilix_rtsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERROR_MAX 160

struct knvr_watch {
    krtsp_source *source;
    krtsp_source *recorder;   /* RECORD-only, main stream, never decodes */
    kmd_detector *detector;
    int width;
    int height;
    int scaled_width;
    int scaled_height;
    bool borrowed;
    knvr_watch_stats stats;
    char error[ERROR_MAX];
};

static bool fail(knvr_watch *watch, const char *reason)
{
    if (watch != NULL) {
        (void)snprintf(watch->error, sizeof(watch->error), "%s", reason);
    }
    return false;
}

const char *knvr_watch_error(const knvr_watch *watch)
{
    if (watch == NULL || watch->error[0] == '\0') {
        return NULL;
    }
    return watch->error;
}

void knvr_watch_options_init(knvr_watch_options *options)
{
    if (options == NULL) {
        return;
    }
    (void)memset(options, 0, sizeof(*options));
    /* The camera's substream shape.  Measured: 320 wide loses subjects
     * under about 50 px, and the substream costs nothing extra because
     * the camera is already producing it. */
    options->width = 640;
    options->height = 360;
    options->fps_cap = 0;
}

/*
 * Load a mask and hand the detector its exclusion form.
 *
 * The mask is painted at whatever resolution suited the operator; the
 * detector needs one byte per *decoded* pixel.  A mask whose source size
 * does not match the decode is refused rather than stretched: a stretched
 * mask silently protects the wrong part of the scene, and the failure mode
 * is a camera that ignores the wrong tree.
 */
static bool apply_mask(knvr_watch *watch, const char *path, int width,
                       int height)
{
    kmask *mask = NULL;
    uint8_t *bytes = NULL;
    const size_t size = (size_t)width * (size_t)height;
    bool ok = false;

    if (!kmask_load(&mask, path)) {
        return fail(watch, "that mask is not one kilix-mask wrote");
    }
    if (kmask_source_width(mask) != width ||
        kmask_source_height(mask) != height) {
        kmask_free(mask);
        return fail(watch,
                    "the mask was painted for a different frame size");
    }
    bytes = malloc(size);
    if (bytes == NULL) {
        kmask_free(mask);
        return fail(watch, "out of memory");
    }
    /* Region 1 is what the operator painted, and this yields 0 there -
     * exactly what the detector reads as "ignore". */
    if (kmask_expand_exclude(mask, 1u, bytes, size)) {
        ok = kmd_detector_set_mask(watch->detector, bytes, size);
    }
    free(bytes);
    kmask_free(mask);
    return ok ? true : fail(watch, "the mask could not be applied");
}

bool knvr_watch_start(
    knvr_watch **out, const char *url, const knvr_watch_options *options)
{
    knvr_watch_options defaults;
    knvr_watch *watch;
    krtsp_source_options source_options;
    kmd_config motion;

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    if (url == NULL || url[0] == '\0') {
        return false;
    }
    if (options == NULL) {
        knvr_watch_options_init(&defaults);
        options = &defaults;
    }
    if (options->width <= 0 || options->height <= 0) {
        return false;
    }
    watch = calloc(1u, sizeof(*watch));
    if (watch == NULL) {
        return false;
    }
    watch->width = options->width;
    watch->height = options->height;

    kmd_config_init(&motion);
    motion.width = options->width;
    motion.height = options->height;
    /* BGRA from the source lands straight in an sr_canvas on
     * little-endian, and the detector reads the same order. */
    motion.pixfmt = KMD_PIXFMT_BGRA;
    if (!kmd_detector_create(&watch->detector, &motion)) {
        free(watch);
        return false;
    }
    watch->scaled_width = kmd_detector_scaled_width(watch->detector);
    watch->scaled_height = kmd_detector_scaled_height(watch->detector);

    if (options->mask_path != NULL && options->mask_path[0] != '\0') {
        if (!apply_mask(watch, options->mask_path, options->width,
                        options->height)) {
            /* Kept so the caller can report it, then torn down: running
             * unmasked because a mask failed to load is how a camera
             * quietly starts detecting the thing it was told to ignore. */
            char reason[ERROR_MAX];

            (void)snprintf(reason, sizeof(reason), "%s", watch->error);
            kmd_detector_free(watch->detector);
            free(watch);
            *out = NULL;
            (void)reason;
            return false;
        }
    }

    krtsp_source_options_init(&source_options);
    source_options.width = options->width;
    source_options.height = options->height;
    source_options.fps_cap = options->fps_cap;
    source_options.pixfmt = KRTSP_PIXFMT_BGRA;
    source_options.letterbox = true;
    source_options.low_latency = true;
    source_options.log_path = options->log_path;
    if (options->record_dir != NULL && options->record_dir[0] != '\0' &&
        (options->record_url == NULL || options->record_url[0] == '\0')) {
        /* No separate archive stream: both roles on this process, which
         * means the archive is whatever this decodes. */
        source_options.roles = KRTSP_ROLE_DECODE | KRTSP_ROLE_RECORD;
        source_options.record_dir = options->record_dir;
        source_options.segment_seconds = options->segment_seconds;
        source_options.record_audio = options->record_audio;
    }
    if (!krtsp_source_start(&watch->source, url, &source_options)) {
        kmd_detector_free(watch->detector);
        free(watch);
        return false;
    }

    if (options->record_dir != NULL && options->record_dir[0] != '\0' &&
        options->record_url != NULL && options->record_url[0] != '\0') {
        krtsp_source_options archive;

        /* RECORD without DECODE: the segmenter copies the camera's own
         * bitstream, so this second process costs I/O and no CPU.  It is
         * what keeps the archive full quality while motion differences
         * the substream. */
        krtsp_source_options_init(&archive);
        archive.roles = KRTSP_ROLE_RECORD;
        archive.width = options->width;
        archive.height = options->height;
        archive.record_dir = options->record_dir;
        archive.segment_seconds = options->segment_seconds;
        archive.record_audio = options->record_audio;
        archive.log_path = options->log_path;
        if (!krtsp_source_start(&watch->recorder, options->record_url,
                                &archive)) {
            /* Losing the archive must not lose the detection: the camera
             * carries on and the caller is told. */
            (void)fail(watch, "the archive stream could not be started");
            watch->recorder = NULL;
        }
    }
    *out = watch;
    return true;
}

void knvr_watch_stop(knvr_watch *watch)
{
    if (watch == NULL) {
        return;
    }
    if (watch->borrowed) {
        krtsp_source_release(watch->source);
    }
    krtsp_source_stop(watch->source);
    krtsp_source_stop(watch->recorder);
    kmd_detector_free(watch->detector);
    free(watch);
}

int knvr_watch_width(const knvr_watch *watch)
{
    return watch != NULL ? watch->width : 0;
}

int knvr_watch_height(const knvr_watch *watch)
{
    return watch != NULL ? watch->height : 0;
}

bool knvr_watch_step(
    knvr_watch *watch, const uint8_t **rgba, knvr_box *boxes,
    size_t capacity, size_t *count)
{
    kmd_box found[KNVR_MOTION_BOX_MAX];
    kmd_result result;
    const uint8_t *frame;
    int age_ms = 0;
    size_t written;

    if (count != NULL) {
        *count = 0u;
    }
    if (watch == NULL) {
        return false;
    }
    if (watch->borrowed) {
        krtsp_source_release(watch->source);
        watch->borrowed = false;
    }
    frame = krtsp_source_borrow(watch->source, &age_ms);
    if (frame == NULL) {
        return false;   /* nothing yet; ordinary during startup */
    }
    watch->borrowed = true;
    watch->stats.frames++;
    watch->stats.last_age_ms = age_ms;
    if (rgba != NULL) {
        *rgba = frame;
    }

    written = kmd_detect(watch->detector, frame, found, KNVR_MOTION_BOX_MAX,
                         &result);
    /* While the background model is converging its boxes are noise, and
     * acting on them produces a burst of false motion at every startup
     * and every reconnect. */
    if (result.calibrating) {
        return true;
    }
    if (written > 0u) {
        watch->stats.motion_frames++;
        watch->stats.boxes += written;
    }

    /* Already in source coordinates; only the half-open corners become a
     * width and a height, and the clamp guards a caller that configured a
     * frame size the detector was not built for. */
    for (size_t i = 0u; i < written && i < capacity; i++) {
        int x0 = found[i].x0 < 0 ? 0 : found[i].x0;
        int y0 = found[i].y0 < 0 ? 0 : found[i].y0;
        int x1 = found[i].x1 > watch->width ? watch->width : found[i].x1;
        int y1 = found[i].y1 > watch->height ? watch->height : found[i].y1;

        boxes[i].x = x0;
        boxes[i].y = y0;
        boxes[i].w = x1 > x0 ? x1 - x0 : 0;
        boxes[i].h = y1 > y0 ? y1 - y0 : 0;
    }
    if (count != NULL) {
        *count = written < capacity ? written : capacity;
    }
    return true;
}

void knvr_watch_get_stats(const knvr_watch *watch, knvr_watch_stats *out)
{
    if (out == NULL) {
        return;
    }
    if (watch == NULL) {
        (void)memset(out, 0, sizeof(*out));
        return;
    }
    *out = watch->stats;
}

/* A one-pixel outline, written straight into the frame.  Deliberately not
 * soft-raster: this runs on the BGRA frame the source produced, before
 * anything has decided whether there is a terminal to draw on at all. */
void knvr_watch_draw_boxes(
    uint8_t *rgba, int width, int height, const knvr_box *boxes,
    size_t count)
{
    if (rgba == NULL || boxes == NULL || width <= 0 || height <= 0) {
        return;
    }
    for (size_t i = 0u; i < count; i++) {
        const knvr_box *box = &boxes[i];
        const int x1 = box->x + box->w - 1;
        const int y1 = box->y + box->h - 1;

        if (box->w <= 0 || box->h <= 0) {
            continue;
        }
        for (int x = box->x; x <= x1 && x < width; x++) {
            for (int edge = 0; edge < 2; edge++) {
                const int y = edge == 0 ? box->y : y1;
                uint8_t *pixel;

                if (x < 0 || y < 0 || y >= height) {
                    continue;
                }
                pixel = rgba + ((size_t)y * (size_t)width + (size_t)x) * 4u;
                pixel[0] = 0x40; pixel[1] = 0xFF; pixel[2] = 0x40;
            }
        }
        for (int y = box->y; y <= y1 && y < height; y++) {
            for (int edge = 0; edge < 2; edge++) {
                const int x = edge == 0 ? box->x : x1;
                uint8_t *pixel;

                if (y < 0 || x < 0 || x >= width) {
                    continue;
                }
                pixel = rgba + ((size_t)y * (size_t)width + (size_t)x) * 4u;
                pixel[0] = 0x40; pixel[1] = 0xFF; pixel[2] = 0x40;
            }
        }
    }
}
