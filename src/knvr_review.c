/*
 * Reviewing what happened.
 *
 * A list of events on the left, the frame that caused the selected one on
 * the right.  That layout is the whole point: an event is only worth
 * anything if you can see what triggered it without leaving the list, and
 * a recorder that makes you open files in another program to answer "was
 * that a person" is a recorder people stop checking.
 *
 * Drawn on the same kitty stack the rest of the fleet uses, so it works
 * over ssh and on a machine with no X server.
 */

#include "knvr_review.h"

#include "kitty_terminal_session.h"
#include "soft_raster.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LIST_WIDTH 340
#define ROW_HEIGHT 18
#define EVENTS_MAX 512

#define BACKDROP 0x00101014u
#define PANEL 0x00181820u
#define TEXT 0x00C8C8D4u
#define DIM 0x00808894u
#define PICKED 0x002A4A66u
#define MARK 0x00FFB020u
#define PERSON 0x0060FF80u

static void format_when(int64_t at, char *out, size_t size)
{
    const time_t when = (time_t)at;
    struct tm parts;

    if (localtime_r(&when, &parts) == NULL) {
        (void)snprintf(out, size, "?");
        return;
    }
    (void)strftime(out, size, "%m-%d %H:%M:%S", &parts);
}

/* The still belonging to an event, as a canvas, or false when there is
 * none - which is ordinary: a camera set to record nothing still raises
 * events. */
static bool load_still(const knvr_store *store, int64_t event_id,
                       sr_canvas *canvas)
{
    knvr_media media[8];
    size_t count = 0u;

    if (!knvr_store_media(store, event_id, media, 8u, &count)) {
        return false;
    }
    for (size_t i = 0u; i < count && i < 8u; i++) {
        if (strcmp(media[i].kind, "still") == 0 &&
            sr_load_ppm(canvas, media[i].path)) {
            return true;
        }
    }
    return false;
}

static void draw_list(
    sr_canvas *frame, const knvr_event *events, size_t count, size_t picked,
    size_t top)
{
    char line[128];
    int y = 8;

    sr_fill_rect(frame, 0.0f, 0.0f, (float)LIST_WIDTH, (float)frame->h,
                 PANEL, 1.0f);
    for (size_t i = top; i < count && y + ROW_HEIGHT < frame->h - 24; i++) {
        const knvr_event *event = &events[i];
        char when[32];
        uint32_t colour = TEXT;

        if (i == picked) {
            sr_fill_rect(frame, 0.0f, (float)(y - 3), (float)LIST_WIDTH,
                         (float)ROW_HEIGHT, PICKED, 1.0f);
        }
        format_when(event->started, when, sizeof(when));
        if (event->best_score > 0.0) {
            /* What it was matters more than that something moved, so the
             * label is what the eye lands on. */
            colour = strcmp(event->best_label, "person") == 0 ? PERSON
                                                              : MARK;
            (void)snprintf(line, sizeof(line), "%-14s %-10s %s %.2f",
                           when, event->camera, event->best_label,
                           event->best_score);
        } else {
            (void)snprintf(line, sizeof(line), "%-14s %-10s motion %llds",
                           when, event->camera,
                           (long long)(event->ended > 0
                                           ? event->ended - event->started
                                           : 0));
            colour = DIM;
        }
        sr_text(frame, 8.0f, (float)y, line, colour, 1.0f, 1);
        y += ROW_HEIGHT;
    }
    if (count == 0u) {
        sr_text(frame, 8.0f, 8.0f, "nothing happened", DIM, 1.0f, 1);
    }
}

static void draw_still(sr_canvas *frame, const sr_canvas *still,
                       const knvr_event *event)
{
    const int area_x = LIST_WIDTH + 8;
    const int area_w = frame->w - area_x - 8;
    const int area_h = frame->h - 40;

    if (still == NULL || still->px == NULL) {
        sr_text(frame, (float)area_x, 8.0f, "no still for this event", DIM,
                1.0f, 1);
        return;
    }
    {
        /* Fit, never crop: a review picture that has had its edges cut
         * off is a review picture that can hide the thing you are looking
         * for. */
        const float scale_x = (float)area_w / (float)still->w;
        const float scale_y = (float)area_h / (float)still->h;
        const float scale = scale_x < scale_y ? scale_x : scale_y;
        const int width = (int)((float)still->w * scale);
        const int height = (int)((float)still->h * scale);

        sr_blit_scaled(frame, still, area_x, 8, width, height, 1.0f);
        if (event != NULL) {
            char caption[96];

            (void)snprintf(caption, sizeof(caption), "event %lld  %s",
                           (long long)event->id, event->camera);
            sr_text(frame, (float)area_x, (float)(8 + height + 6), caption,
                    DIM, 1.0f, 1);
        }
    }
}

int knvr_review(knvr_store *store)
{
    kittyts_session session;
    kittyts_options options;
    sr_canvas frame;
    sr_canvas still;
    knvr_event *events;
    uint8_t *rgba = NULL;
    size_t count = 0u;
    size_t picked = 0u;
    size_t top = 0u;
    int64_t shown = -1;
    bool have_still = false;
    bool running = true;
    bool dirty = true;
    int width;
    int height;
    int status = 0;

    if (store == NULL) {
        return 1;
    }
    events = calloc(EVENTS_MAX, sizeof(*events));
    if (events == NULL) {
        return 1;
    }
    if (!knvr_store_events(store, NULL, events, EVENTS_MAX, &count)) {
        free(events);
        return 1;
    }
    if (count > EVENTS_MAX) {
        count = EVENTS_MAX;
    }
    (void)memset(&still, 0, sizeof(still));

    kittyts_session_init(&session);
    kittyts_options_init(&options);
    if (kittyts_start(&session, STDIN_FILENO, STDOUT_FILENO, &options) != 0) {
        (void)fprintf(stderr, "kilix-nvr: %s\n",
                      errno == ENOTSUP
                          ? "this terminal does not support graphics"
                          : strerror(errno));
        free(events);
        return 1;
    }
    width = kittyts_width(&session);
    height = kittyts_height(&session);
    if (!sr_canvas_init(&frame, width, height)) {
        kittyts_stop(&session);
        free(events);
        return 1;
    }
    rgba = malloc((size_t)width * (size_t)height * 4u);
    if (rgba == NULL) {
        sr_canvas_free(&frame);
        kittyts_stop(&session);
        free(events);
        return 1;
    }

    while (running) {
        struct pollfd descriptor = {STDIN_FILENO, POLLIN, 0};
        kittykb_event key;

        if (dirty) {
            const knvr_event *event = count > 0u ? &events[picked] : NULL;

            /* Loaded on selection rather than up front: a day of events
             * is a day of images, and the reviewer looks at one. */
            if (event != NULL && event->id != shown) {
                if (have_still) {
                    sr_canvas_free(&still);
                    have_still = false;
                }
                have_still = load_still(store, event->id, &still);
                shown = event->id;
            }
            sr_clear(&frame, BACKDROP);
            draw_list(&frame, events, count, picked, top);
            draw_still(&frame, have_still ? &still : NULL, event);
            sr_text(&frame, 8.0f, (float)(height - 16),
                    "up/down select   q quit", DIM, 1.0f, 1);
            if (sr_pack_rgba(&frame, rgba,
                             (size_t)width * (size_t)height * 4u)) {
                (void)kittyts_present(&session, rgba, width, height);
            }
            dirty = false;
        }
        if (poll(&descriptor, 1u, 60) > 0) {
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
            if (key.key == KITTYKB_KEY_DOWN || key.key == 'j') {
                if (count > 0u && picked + 1u < count) {
                    picked++;
                }
                dirty = true;
            } else if (key.key == KITTYKB_KEY_UP || key.key == 'k') {
                if (picked > 0u) {
                    picked--;
                }
                dirty = true;
            }
            {
                /* Keep the selection on screen without scrolling for the
                 * sake of it. */
                const size_t rows =
                    (size_t)((height - 32) / ROW_HEIGHT);

                if (picked < top) {
                    top = picked;
                } else if (rows > 0u && picked >= top + rows) {
                    top = picked - rows + 1u;
                }
            }
        }
    }

    if (have_still) {
        sr_canvas_free(&still);
    }
    free(rgba);
    sr_canvas_free(&frame);
    kittyts_stop(&session);
    free(events);
    return status;
}
