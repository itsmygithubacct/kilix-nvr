/*
 * The viewer.
 *
 * A feed per camera on screen: frames from kilix-rtsp, motion from
 * kilix-motion-detect, boxes from kilix-object-detect on the one being
 * watched, level from kilix-sound-detect.  Every second, the peak of the
 * first and the last goes into the store as a pulse row, so the strips
 * survive the program and a day can be scrolled back through.
 *
 * The expensive thing is deliberately not uniform: every feed differences
 * frames and measures loudness, because both are arithmetic, and only the
 * focused feed runs a detector, because a detector is hundreds of
 * megabytes.  A grid of nine cameras that each loaded a model would be a
 * grid nobody could open.
 */

#include "knvr_view.h"

#include "knvr_paths.h"
#include "knvr_strip.h"
#include "knvr_track.h"

#include "kilix_motion_detect.h"
#include "kilix_object_detect.h"
#include "kilix_rtsp.h"
#include "kilix_sound_detect.h"
#include "kitty_terminal_session.h"
#include "soft_raster.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FEED_MAX 9
#define HISTORY 120            /* seconds of live strip */
#define EVENTS_SHOWN 64
#define DECODE_W 640
#define DECODE_H 360
#define MOTION_HEIGHT 100
#define STRIP_H 26

#define BACKDROP 0x00101014u
#define PANEL 0x00181820u
#define TEXT 0x00C8C8D4u
#define DIM 0x00707884u
#define PICKED 0x002A4A66u
#define MOTION_COLOUR 0x0060B0FFu
#define SOUND_COLOUR 0x0060FF80u
#define LIVE 0x0060FF80u

typedef struct feed {
    char name[KNVR_NAME_MAX];
    krtsp_source *source;
    kmd_detector *motion;
    kod_detector *detector;
    ksd_listener *sound;
    knvr_ring motion_ring;
    knvr_ring sound_ring;
    kod_box boxes[KOD_BOX_MAX];
    size_t box_count;
    uint8_t *frame;            /* the last frame, with overlays drawn */
    uint64_t seen;             /* source frames at the last borrow */
    uint64_t frames;
    int64_t second;            /* the second being accumulated */
    float motion_peak;
    float sound_peak;
    float scores[KSD_CLASS_COUNT];
    bool online;
} feed;

typedef struct view {
    feed feeds[FEED_MAX];
    size_t count;
    size_t focus;
    bool grid;
    knvr_store *store;
    knvr_event events[EVENTS_SHOWN];
    size_t event_count;
    size_t event_picked;
    time_t events_read;
    bool detect;
} view;

/* ------------------------------- the feeds ------------------------------- */

static bool resolve_url(const char *name, char *out, size_t size)
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

        if (camera == NULL || strcmp(camera->name, name) != 0) {
            continue;
        }
        /* The substream: this is a view, and the archive is somebody
         * else's job. */
        if (camera->url_sub[0] != '\0' && strlen(camera->url_sub) < size) {
            (void)snprintf(out, size, "%s", camera->url_sub);
            found = true;
        } else if (strlen(camera->url_main) < size) {
            (void)snprintf(out, size, "%s", camera->url_main);
            found = true;
        }
        break;
    }
    krtsp_config_free(config);
    return found;
}

static void feed_stop(feed *at)
{
    ksd_close(at->sound);
    kod_close(at->detector);
    kmd_detector_free(at->motion);
    krtsp_source_stop(at->source);
    knvr_ring_free(&at->motion_ring);
    knvr_ring_free(&at->sound_ring);
    free(at->frame);
    (void)memset(at, 0, sizeof(*at));
}

static bool feed_start(feed *at, const char *name, bool listen)
{
    krtsp_source_options source_options;
    kmd_config motion_config;
    ksd_options sound_options;
    char url[KRTSP_URL_MAX];
    char log_path[KNVR_PATH_MAX];

    (void)memset(at, 0, sizeof(*at));
    (void)snprintf(at->name, sizeof(at->name), "%.*s",
                   (int)sizeof(at->name) - 1, name);
    if (!resolve_url(name, url, sizeof(url))) {
        return false;
    }
    krtsp_source_options_init(&source_options);
    source_options.pixfmt = KRTSP_PIXFMT_BGRA;
    source_options.width = DECODE_W;
    source_options.height = DECODE_H;
    source_options.letterbox = true;
    if (knvr_paths_state_file(log_path, sizeof(log_path), "ffmpeg.log")) {
        source_options.log_path = log_path;
    }
    if (!krtsp_source_start(&at->source, url, &source_options)) {
        return false;
    }
    kmd_config_init(&motion_config);
    motion_config.width = DECODE_W;
    motion_config.height = DECODE_H;
    motion_config.pixfmt = KMD_PIXFMT_BGRA;
    motion_config.detect_height = MOTION_HEIGHT;
    if (!kmd_detector_create(&at->motion, &motion_config)) {
        feed_stop(at);
        return false;
    }
    at->frame = malloc((size_t)DECODE_W * (size_t)DECODE_H * 4u);
    if (at->frame == NULL ||
        !knvr_ring_init(&at->motion_ring, HISTORY) ||
        !knvr_ring_init(&at->sound_ring, HISTORY)) {
        feed_stop(at);
        return false;
    }
    if (listen) {
        ksd_options_init(&sound_options);
        sound_options.min_score = 0.0f;   /* the view thresholds */
        if (knvr_paths_state_file(log_path, sizeof(log_path),
                                  "ffmpeg-audio.log")) {
            sound_options.log_path = log_path;
        }
        /* Both subprocesses muzzled: this program owns the screen, and one
         * line from a model loader corrupts it. */
        if (knvr_paths_state_file(log_path, sizeof(log_path),
                                  "classify.log")) {
            sound_options.classifier_log_path = log_path;
        }
        (void)ksd_open(&at->sound, url, &sound_options);
    }
    return true;
}

/* The detector follows the focus: started when a feed becomes the one
 * being watched, stopped when it stops being. */
static void feed_detector(feed *at, bool wanted)
{
    kod_options options;
    char log_path[KNVR_PATH_MAX];

    if (wanted == (at->detector != NULL)) {
        return;
    }
    if (!wanted) {
        kod_close(at->detector);
        at->detector = NULL;
        at->box_count = 0u;
        return;
    }
    kod_options_init(&options);
    options.size = 320;
    if (knvr_paths_state_file(log_path, sizeof(log_path), "detect.log")) {
        options.log_path = log_path;
    }
    (void)kod_open(&at->detector, &options);
}

/*
 * One frame through one feed.
 *
 * Returns true when a new frame arrived.  The pulse is accumulated as a
 * peak within the second and written when the second turns over, so the
 * store sees one row a second however fast the camera is.
 */
static bool feed_step(feed *at, knvr_store *store)
{
    krtsp_source_stats stats;
    const uint8_t *pixels;
    kmd_box motion[32];
    kmd_result result;
    kod_rect moved[32];
    kod_rect crops[KOD_REGION_MAX];
    size_t moved_count;
    size_t crop_count;
    int age_ms = 0;
    const int64_t now = (int64_t)time(NULL);

    if (at->sound != NULL) {
        ksd_event heard[KSD_CLASS_COUNT];
        size_t count = 0u;

        if (ksd_step(at->sound, heard, KSD_CLASS_COUNT, &count)) {
            const float *scores = ksd_scores(at->sound);
            const float level = ksd_level(at->sound);

            if (scores != NULL) {
                (void)memcpy(at->scores, scores, sizeof(at->scores));
            }
            if (level > at->sound_peak) {
                at->sound_peak = level;
            }
        } else if (ksd_error(at->sound) != NULL) {
            ksd_close(at->sound);
            at->sound = NULL;
        }
    }
    krtsp_source_get_stats(at->source, &stats);
    if (stats.frames == at->seen) {
        /* Nothing new.  Borrowing anyway would hand back the same frame
         * and difference it against itself, which reads as a camera that
         * has stopped moving. */
        if (at->second != 0 && now != at->second) {
            (void)knvr_store_pulse(store, at->name, at->second,
                                   at->motion_peak, at->sound_peak);
            knvr_ring_push(&at->motion_ring, at->motion_peak);
            knvr_ring_push(&at->sound_ring, at->sound_peak);
            at->motion_peak = 0.0f;
            at->sound_peak = 0.0f;
            at->second = now;
        }
        return false;
    }
    pixels = krtsp_source_borrow(at->source, &age_ms);
    if (pixels == NULL) {
        return false;
    }
    at->seen = stats.frames;
    at->frames++;
    at->online = true;
    (void)memcpy(at->frame, pixels,
                 (size_t)DECODE_W * (size_t)DECODE_H * 4u);
    moved_count = kmd_detect(at->motion, pixels, motion,
                             sizeof(motion) / sizeof(motion[0]), &result);
    krtsp_source_release(at->source);

    if (result.motion_fraction > at->motion_peak && !result.calibrating) {
        at->motion_peak = result.motion_fraction;
    }
    if (at->second == 0) {
        at->second = now;
    } else if (now != at->second) {
        (void)knvr_store_pulse(store, at->name, at->second, at->motion_peak,
                               at->sound_peak);
        knvr_ring_push(&at->motion_ring, at->motion_peak);
        knvr_ring_push(&at->sound_ring, at->sound_peak);
        at->motion_peak = 0.0f;
        at->sound_peak = 0.0f;
        at->second = now;
    }

    at->box_count = 0u;
    if (at->detector != NULL && moved_count > 0u) {
        for (size_t i = 0u; i < moved_count && i < 32u; i++) {
            moved[i].x = motion[i].x0;
            moved[i].y = motion[i].y0;
            moved[i].w = motion[i].x1 - motion[i].x0;
            moved[i].h = motion[i].y1 - motion[i].y0;
        }
        crop_count = kod_regions(moved, moved_count, DECODE_W, DECODE_H, 320,
                                 crops, KOD_REGION_MAX, NULL);
        if (crop_count > 0u) {
            (void)kod_detect_regions(at->detector, at->frame, DECODE_W,
                                     DECODE_H, crops, crop_count, at->boxes,
                                     KOD_BOX_MAX, &at->box_count);
        }
    }
    /* Outlines from the module, so the recorder and the analyzer cannot
     * draw a person a different colour. */
    kod_draw_boxes(at->frame, DECODE_W, DECODE_H, at->boxes, at->box_count);
    return true;
}

/* -------------------------------- drawing -------------------------------- */

static void draw_labels(sr_canvas *canvas, const feed *at, int x, int y,
                        int width, int height)
{
    for (size_t i = 0u; i < at->box_count; i++) {
        const kod_box *box = &at->boxes[i];
        const char *label = kod_label(box->class_id);
        char text[64];
        float bx;
        float by;

        if (label == NULL) {
            continue;
        }
        (void)snprintf(text, sizeof(text), "%s %.2f", label,
                       (double)box->score);
        /* Scaled here, once, in the same place the picture was scaled:
         * two scalings in two functions is how boxes end up beside the
         * thing they are meant to be around. */
        bx = (float)x + (float)box->at.x * (float)width / (float)DECODE_W;
        by = (float)y + (float)box->at.y * (float)height / (float)DECODE_H;
        if (by < (float)y + 14.0f) {
            by += 16.0f;
        } else {
            by -= 14.0f;
        }
        sr_fill_rect(canvas, bx, by,
                     (float)sr_text_width_in(SR_FONT_FIXED_8X16, text, 1) +
                         4.0f,
                     14.0f, 0x000000u, 0.55f);
        sr_text(canvas, bx + 2.0f, by, text,
                kod_class_colour(box->class_id), 1.0f, 1);
    }
}

static void draw_picture(sr_canvas *canvas, const feed *at, int x, int y,
                         int width, int height)
{
    sr_canvas picture;
    float scale;
    int drawn_w;
    int drawn_h;

    if (at->frame == NULL || !at->online) {
        sr_fill_rect(canvas, (float)x, (float)y, (float)width, (float)height,
                     PANEL, 1.0f);
        sr_text(canvas, (float)(x + 8), (float)(y + height / 2),
                at->source != NULL ? "waiting for a frame" : "offline", DIM,
                1.0f, 1);
        return;
    }
    {
        const float sx = (float)width / (float)DECODE_W;
        const float sy = (float)height / (float)DECODE_H;

        scale = sx < sy ? sx : sy;
        drawn_w = (int)((float)DECODE_W * scale);
        drawn_h = (int)((float)DECODE_H * scale);
    }
    sr_canvas_wrap(&picture, (uint32_t *)(void *)(uintptr_t)at->frame,
                   DECODE_W, DECODE_H);
    sr_blit_scaled(canvas, &picture, x, y, drawn_w, drawn_h, 1.0f);
    draw_labels(canvas, at, x, y, drawn_w, drawn_h);
}

static void draw_strips(sr_canvas *canvas, const feed *at, int x, int y,
                        int width, bool compact)
{
    float motion[HISTORY];
    float sound[HISTORY];
    knvr_strip strip;

    knvr_ring_read(&at->motion_ring, motion, HISTORY);
    knvr_ring_read(&at->sound_ring, sound, HISTORY);

    (void)memset(&strip, 0, sizeof(strip));
    strip.label = compact ? NULL : "motion";
    strip.samples = motion;
    strip.count = HISTORY;
    strip.colour = MOTION_COLOUR;
    strip.threshold = -1.0f;
    strip.cursor = -1.0f;
    knvr_strip_draw(canvas, &strip, x, y, width,
                    compact ? STRIP_H / 2 : STRIP_H);

    strip.label = compact ? NULL : "sound";
    strip.samples = sound;
    strip.colour = SOUND_COLOUR;
    /* The default the classifier is thresholded at, drawn so the number
     * is a thing you can see rather than one you have to remember. */
    strip.threshold = 0.5f;
    knvr_strip_draw(canvas, &strip, x, y + (compact ? STRIP_H / 2 : STRIP_H),
                    width, compact ? STRIP_H / 2 : STRIP_H);
}

static void draw_events(sr_canvas *canvas, const view *state, int x, int y,
                        int width, int height)
{
    char line[160];
    int row = y;

    sr_fill_rect(canvas, (float)x, (float)y, (float)width, (float)height,
                 PANEL, 1.0f);
    sr_text(canvas, (float)(x + 8), (float)(y + 6), "what happened", DIM,
            1.0f, 1);
    row += 24;
    if (state->event_count == 0u) {
        sr_text(canvas, (float)(x + 8), (float)row, "nothing yet", DIM, 1.0f,
                1);
        return;
    }
    for (size_t i = 0u; i < state->event_count && row + 16 < y + height;
         i++) {
        const knvr_event *event = &state->events[i];
        const time_t when = (time_t)event->started;
        struct tm parts;
        char stamp[16] = "?";
        uint32_t colour = DIM;

        if (localtime_r(&when, &parts) != NULL) {
            (void)strftime(stamp, sizeof(stamp), "%H:%M:%S", &parts);
        }
        if (i == state->event_picked) {
            sr_fill_rect(canvas, (float)x, (float)(row - 3), (float)width,
                         18.0f, PICKED, 1.0f);
        }
        if (event->best_score > 0.0) {
            colour = strcmp(event->best_label, "person") == 0 ? LIVE
                                                              : 0x00FFB020u;
            (void)snprintf(line, sizeof(line), "%s %-10s %s %.2f", stamp,
                           event->camera, event->best_label,
                           event->best_score);
        } else {
            (void)snprintf(line, sizeof(line), "%s %-10s motion", stamp,
                           event->camera);
        }
        sr_text(canvas, (float)(x + 8), (float)row, line, colour, 1.0f, 1);
        row += 18;
    }
}

static void refresh_events(view *state)
{
    knvr_query query;

    if (time(NULL) == state->events_read) {
        return;
    }
    state->events_read = time(NULL);
    knvr_query_init(&query);
    query.limit = EVENTS_SHOWN;
    (void)knvr_store_events(state->store, &query, state->events,
                            EVENTS_SHOWN, &state->event_count);
    if (state->event_count > EVENTS_SHOWN) {
        state->event_count = EVENTS_SHOWN;
    }
    if (state->event_picked >= state->event_count) {
        state->event_picked = 0u;
    }
}

static void compose(sr_canvas *canvas, view *state)
{
    char line[256];
    const int width = canvas->w;
    const int height = canvas->h;
    const int list_width = width / 4 < 280 ? 280 : width / 4;
    const int stage_w = width - list_width - 16;

    sr_clear(canvas, BACKDROP);
    sr_fill_rect(canvas, 0.0f, 0.0f, (float)width, 26.0f, PANEL, 1.0f);
    if (state->grid) {
        (void)snprintf(line, sizeof(line), "%zu cameras", state->count);
    } else {
        const feed *at = &state->feeds[state->focus];

        (void)snprintf(line, sizeof(line), "%s%s", at->name,
                       at->detector != NULL ? "   detecting" : "");
    }
    sr_text(canvas, 8.0f, 7.0f, line, TEXT, 1.0f, 1);
    sr_text(canvas,
            (float)(width - 8 -
                    sr_text_width_in(SR_FONT_FIXED_8X16,
                                     "q quit  tab camera  g grid  d detect",
                                     1)),
            7.0f, "q quit  tab camera  g grid  d detect", DIM, 1.0f, 1);

    if (state->grid) {
        /* Two across is the readable maximum at this size; more cameras
         * make more rows and smaller tiles rather than a wall of stamps. */
        const size_t columns = state->count > 1u ? 2u : 1u;
        const size_t rows = (state->count + columns - 1u) / columns;
        const int tile_w = (stage_w - 8) / (int)columns;
        const int tile_h = (height - 40) / (int)rows;

        for (size_t i = 0u; i < state->count; i++) {
            const int x = 8 + (int)(i % columns) * tile_w;
            const int y = 32 + (int)(i / columns) * tile_h;
            const int picture_h = tile_h - STRIP_H - 20;

            sr_text(canvas, (float)x, (float)y, state->feeds[i].name,
                    i == state->focus ? TEXT : DIM, 1.0f, 1);
            draw_picture(canvas, &state->feeds[i], x, y + 16, tile_w - 8,
                         picture_h);
            draw_strips(canvas, &state->feeds[i], x, y + 16 + picture_h + 2,
                        tile_w - 8, true);
        }
    } else {
        const int picture_h = height - 40 - 2 * STRIP_H - 8;

        draw_picture(canvas, &state->feeds[state->focus], 8, 32, stage_w,
                     picture_h);
        draw_strips(canvas, &state->feeds[state->focus], 8,
                    32 + picture_h + 6, stage_w, false);
    }
    draw_events(canvas, state, width - list_width, 32, list_width - 8,
                height - 40);
}

/* --------------------------------- running ------------------------------- */

void knvr_view_options_init(knvr_view_options *options)
{
    if (options == NULL) {
        return;
    }
    (void)memset(options, 0, sizeof(*options));
    options->detect = true;
}

static bool write_ppm(const char *path, const sr_canvas *canvas)
{
    FILE *file = fopen(path, "wb");

    if (file == NULL) {
        return false;
    }
    (void)fprintf(file, "P6\n%d %d\n255\n", canvas->w, canvas->h);
    for (int i = 0; i < canvas->w * canvas->h; i++) {
        const uint32_t pixel = canvas->px[i];

        (void)fputc((int)((pixel >> 16) & 0xFFu), file);
        (void)fputc((int)((pixel >> 8) & 0xFFu), file);
        (void)fputc((int)(pixel & 0xFFu), file);
    }
    return fclose(file) == 0;
}

int knvr_view(
    knvr_config *config, knvr_store *store, const knvr_view_options *options)
{
    knvr_view_options defaults;
    view state;
    kittyts_session session;
    kittyts_options session_options;
    sr_canvas canvas;
    uint8_t *rgba = NULL;
    knvr_camera cameras[KNVR_CAMERA_MAX];
    size_t configured = 0u;
    time_t deadline;
    time_t last_draw = 0;
    bool running = true;
    bool headless;
    int width = 960;
    int height = 540;
    int status = 0;

    if (config == NULL || store == NULL) {
        return 1;
    }
    if (options == NULL) {
        knvr_view_options_init(&defaults);
        options = &defaults;
    }
    headless = options->render != NULL;
    (void)memset(&state, 0, sizeof(state));
    state.store = store;
    state.detect = options->detect;
    state.grid = options->camera == NULL;

    if (!knvr_config_list(config, cameras, KNVR_CAMERA_MAX, &configured)) {
        return 1;
    }
    for (size_t i = 0u; i < configured && state.count < FEED_MAX; i++) {
        if (options->camera != NULL &&
            strcmp(cameras[i].name, options->camera) != 0) {
            continue;
        }
        if (feed_start(&state.feeds[state.count], cameras[i].name, true)) {
            state.count++;
        }
    }
    if (state.count == 0u) {
        (void)fprintf(stderr,
                      options->camera != NULL
                          ? "kilix-nvr: no such camera, or no stream for it\n"
                          : "kilix-nvr: no cameras configured; add one with "
                            "`kilix-nvr add <name>`\n");
        return 1;
    }
    /* One detector, on what is being watched.  A grid of nine that each
     * loaded a model would be a grid nobody could open. */
    if (state.detect) {
        feed_detector(&state.feeds[state.focus], true);
    }

    if (!headless) {
        kittyts_session_init(&session);
        kittyts_options_init(&session_options);
        if (kittyts_start(&session, STDIN_FILENO, STDOUT_FILENO,
                          &session_options) != 0) {
            (void)fprintf(stderr, "kilix-nvr: %s\n",
                          errno == ENOTSUP
                              ? "this terminal does not support graphics"
                              : strerror(errno));
            for (size_t i = 0u; i < state.count; i++) {
                feed_stop(&state.feeds[i]);
            }
            return 1;
        }
        width = kittyts_width(&session);
        height = kittyts_height(&session);
    }
    if (!sr_canvas_init(&canvas, width, height)) {
        status = 1;
        running = false;
    } else if (!headless) {
        rgba = malloc((size_t)width * (size_t)height * 4u);
        if (rgba == NULL) {
            status = 1;
            running = false;
        }
    }
    deadline = options->seconds > 0 ? time(NULL) + options->seconds : 0;

    while (running) {
        struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
        kittykb_event key;
        bool fresh = false;

        if (deadline > 0 && time(NULL) >= deadline) {
            break;
        }
        for (size_t i = 0u; i < state.count; i++) {
            if (feed_step(&state.feeds[i], store)) {
                fresh = true;
            }
        }
        refresh_events(&state);

        if (headless) {
            /* One frame, once every feed has something to show - a
             * picture of "waiting for a frame" proves only that the
             * program starts. */
            bool ready = true;

            for (size_t i = 0u; i < state.count; i++) {
                /* A frame is not enough: the strips are the point, and
                 * they have nothing in them until a few seconds have
                 * turned over.  A render that proves only "a picture
                 * appeared" is a render that cannot catch an empty
                 * strip. */
                if (!state.feeds[i].online ||
                    state.feeds[i].motion_ring.count < 5u) {
                    ready = false;
                }
            }
            if (ready || (deadline > 0 && time(NULL) + 2 >= deadline)) {
                compose(&canvas, &state);
                status = write_ppm(options->render, &canvas) ? 0 : 1;
                break;
            }
            {
                struct timespec pause = {0, 50 * 1000 * 1000};

                (void)nanosleep(&pause, NULL);
            }
            continue;
        }
        if (fresh || time(NULL) != last_draw) {
            last_draw = time(NULL);
            compose(&canvas, &state);
            if (sr_pack_rgba(&canvas, rgba,
                             (size_t)width * (size_t)height * 4u)) {
                (void)kittyts_present(&session, rgba, width, height);
            }
        }
        if (poll(&descriptor, 1u, 20) > 0) {
            (void)kittyts_read_input(&session);
        }
        while (kittyts_next_key_event(&session, &key)) {
            if (key.action == KITTYKB_ACTION_RELEASE) {
                continue;
            }
            if (key.key == 'q' || key.key == KITTYKB_KEY_ESCAPE) {
                running = false;
                break;
            }
            if (key.key == '\t' || key.key == KITTYKB_KEY_RIGHT) {
                feed_detector(&state.feeds[state.focus], false);
                state.focus = (state.focus + 1u) % state.count;
                feed_detector(&state.feeds[state.focus], state.detect);
                state.grid = false;
            } else if (key.key == KITTYKB_KEY_LEFT) {
                feed_detector(&state.feeds[state.focus], false);
                state.focus = (state.focus + state.count - 1u) % state.count;
                feed_detector(&state.feeds[state.focus], state.detect);
                state.grid = false;
            } else if (key.key == 'g') {
                state.grid = !state.grid;
            } else if (key.key == 'd') {
                state.detect = !state.detect;
                feed_detector(&state.feeds[state.focus], state.detect);
            } else if (key.key == KITTYKB_KEY_DOWN) {
                if (state.event_picked + 1u < state.event_count) {
                    state.event_picked++;
                }
            } else if (key.key == KITTYKB_KEY_UP) {
                if (state.event_picked > 0u) {
                    state.event_picked--;
                }
            }
            last_draw = 0;
        }
    }

    for (size_t i = 0u; i < state.count; i++) {
        feed_stop(&state.feeds[i]);
    }
    free(rgba);
    sr_canvas_free(&canvas);
    if (!headless) {
        kittyts_stop(&session);
    }
    return status;
}
