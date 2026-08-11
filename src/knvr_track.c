/*
 * Matching this frame's detections to the objects already known.
 *
 * Overlap first, then a centroid gate for the ones overlap misses - a
 * subject walking towards a camera can double in size between two frames
 * that are a second apart, and its boxes then barely intersect even
 * though the object plainly did not teleport.  Both stages are restricted
 * to detections of the same class, because a car turning into a person is
 * never the right answer and allowing it makes every label unreliable.
 */

#include "knvr_track.h"

#include <stdlib.h>
#include <string.h>

struct knvr_tracker {
    knvr_track tracks[KNVR_TRACK_MAX];
    bool used[KNVR_TRACK_MAX];
    /* Where the centroid was when it last moved far enough to count, and
     * when that was.  Stationary is the distance from here, not from the
     * previous frame: a car rocking one pixel in the wind would otherwise
     * never settle. */
    knvr_point anchor[KNVR_TRACK_MAX];
    int64_t anchor_at[KNVR_TRACK_MAX];
    int64_t assigned[KNVR_DETECT_ROWS];
    size_t assigned_count;
    int64_t next_id;
    int64_t total;
    knvr_tracker_options options;
};

void knvr_tracker_options_init(knvr_tracker_options *options)
{
    if (options == NULL) {
        return;
    }
    (void)memset(options, 0, sizeof(*options));
    options->iou_min = 0.2f;
    options->min_hits = 2;
    options->max_gap_ms = 5000;
    options->stationary_px = 12;
    options->stationary_ms = 30000;
}

bool knvr_tracker_create(
    knvr_tracker **out, const knvr_tracker_options *options)
{
    knvr_tracker *tracker;

    if (out == NULL) {
        return false;
    }
    *out = NULL;
    tracker = calloc(1u, sizeof(*tracker));
    if (tracker == NULL) {
        return false;
    }
    if (options != NULL) {
        tracker->options = *options;
    } else {
        knvr_tracker_options_init(&tracker->options);
    }
    if (tracker->options.iou_min <= 0.0f) {
        tracker->options.iou_min = 0.2f;
    }
    if (tracker->options.min_hits <= 0) {
        tracker->options.min_hits = 1;
    }
    if (tracker->options.max_gap_ms <= 0) {
        tracker->options.max_gap_ms = 5000;
    }
    tracker->next_id = 1;
    *out = tracker;
    return true;
}

void knvr_tracker_free(knvr_tracker *tracker)
{
    free(tracker);
}

static float overlap(const knvr_detection_box *a, const knvr_detection_box *b)
{
    const int x0 = a->x > b->x ? a->x : b->x;
    const int y0 = a->y > b->y ? a->y : b->y;
    const int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
    const int y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;
    const int width = x1 - x0;
    const int height = y1 - y0;
    long intersection;
    long combined;

    if (width <= 0 || height <= 0) {
        return 0.0f;
    }
    intersection = (long)width * (long)height;
    combined = (long)a->w * (long)a->h + (long)b->w * (long)b->h -
               intersection;
    if (combined <= 0) {
        return 0.0f;
    }
    return (float)((double)intersection / (double)combined);
}

static knvr_point centre(const knvr_detection_box *box)
{
    knvr_point point;

    point.x = box->x + box->w / 2;
    point.y = box->y + box->h / 2;
    return point;
}

static int distance(knvr_point a, knvr_point b)
{
    const long dx = (long)a.x - (long)b.x;
    const long dy = (long)a.y - (long)b.y;
    long squared = dx * dx + dy * dy;
    long root = 0;

    /* Integer square root: this is compared against a pixel threshold, so
     * carrying a double through it would be precision nobody reads. */
    while ((root + 1) * (root + 1) <= squared) {
        root++;
    }
    return (int)root;
}

/* Near enough to be the same object even though the boxes barely touch:
 * within 60% of the larger box's own size. */
static bool near_enough(const knvr_detection_box *track,
                        const knvr_detection_box *seen)
{
    const int reach = (track->w > track->h ? track->w : track->h);
    const int gap = distance(centre(track), centre(seen));

    return reach > 0 && gap * 10 <= reach * 6;
}

static void remember(knvr_tracker *tracker, size_t slot,
                     const knvr_detection_box *box, int64_t at_ms)
{
    knvr_track *track = &tracker->tracks[slot];
    const knvr_point point = centre(box);

    if (track->path_length > 0u) {
        track->travelled +=
            distance(track->path[track->path_length - 1u], point);
    }
    if (track->path_length == KNVR_TRACK_PATH) {
        (void)memmove(&track->path[0], &track->path[1],
                      sizeof(track->path[0]) * (KNVR_TRACK_PATH - 1u));
        track->path_length--;
    }
    track->path[track->path_length++] = point;

    if (distance(point, tracker->anchor[slot]) > tracker->options.stationary_px) {
        tracker->anchor[slot] = point;
        tracker->anchor_at[slot] = at_ms;
        track->stationary = false;
    } else if (at_ms - tracker->anchor_at[slot] >=
               (int64_t)tracker->options.stationary_ms) {
        track->stationary = true;
    }
}

static bool spawn(knvr_tracker *tracker, const knvr_detection_box *box,
                  int64_t at_ms, int64_t *id)
{
    for (size_t slot = 0u; slot < KNVR_TRACK_MAX; slot++) {
        knvr_track *track;

        if (tracker->used[slot]) {
            continue;
        }
        track = &tracker->tracks[slot];
        (void)memset(track, 0, sizeof(*track));
        track->id = tracker->next_id++;
        track->class_id = box->class_id;
        track->score = box->score;
        track->box = *box;
        track->first_seen = at_ms;
        track->last_seen = at_ms;
        track->hits = 1;
        track->confirmed = tracker->options.min_hits <= 1;
        tracker->used[slot] = true;
        tracker->anchor[slot] = centre(box);
        tracker->anchor_at[slot] = at_ms;
        tracker->total++;
        remember(tracker, slot, box, at_ms);
        *id = track->id;
        return true;
    }
    /* Every slot busy.  Refusing to track the newcomer beats evicting a
     * confirmed object to make room for what may be a hallucination. */
    *id = 0;
    return false;
}

bool knvr_tracker_update(
    knvr_tracker *tracker, const knvr_detection_box *boxes, size_t count,
    int64_t at_ms)
{
    bool taken[KNVR_DETECT_ROWS];
    bool matched[KNVR_TRACK_MAX];

    if (tracker == NULL || (boxes == NULL && count > 0u)) {
        return false;
    }
    if (count > KNVR_DETECT_ROWS) {
        count = KNVR_DETECT_ROWS;
    }
    (void)memset(taken, 0, sizeof(taken));
    (void)memset(matched, 0, sizeof(matched));
    tracker->assigned_count = count;
    for (size_t i = 0u; i < KNVR_DETECT_ROWS; i++) {
        tracker->assigned[i] = 0;
    }

    /* Forget what has been gone too long, before matching rather than
     * after: a stale track sitting where an object used to be will happily
     * claim a new one that walks through the same doorway. */
    for (size_t slot = 0u; slot < KNVR_TRACK_MAX; slot++) {
        if (tracker->used[slot] &&
            at_ms - tracker->tracks[slot].last_seen >
                (int64_t)tracker->options.max_gap_ms) {
            tracker->used[slot] = false;
        }
    }

    /* Greedy by overlap: the best pair in the whole frame is settled
     * first, then the next best among what is left.  With at most 32
     * tracks and 20 detections the cost of doing this properly is not
     * worth an assignment algorithm. */
    for (;;) {
        float best_score = tracker->options.iou_min;
        size_t best_slot = KNVR_TRACK_MAX;
        size_t best_box = count;

        for (size_t slot = 0u; slot < KNVR_TRACK_MAX; slot++) {
            if (!tracker->used[slot] || matched[slot]) {
                continue;
            }
            for (size_t i = 0u; i < count; i++) {
                float score;

                if (taken[i] ||
                    boxes[i].class_id != tracker->tracks[slot].class_id) {
                    continue;
                }
                score = overlap(&tracker->tracks[slot].box, &boxes[i]);
                if (score > best_score) {
                    best_score = score;
                    best_slot = slot;
                    best_box = i;
                }
            }
        }
        if (best_slot == KNVR_TRACK_MAX) {
            break;
        }
        matched[best_slot] = true;
        taken[best_box] = true;
        tracker->assigned[best_box] = tracker->tracks[best_slot].id;
    }

    /* What overlap missed, proximity may still catch. */
    for (size_t i = 0u; i < count; i++) {
        size_t best_slot = KNVR_TRACK_MAX;
        int best_gap = 0;

        if (taken[i]) {
            continue;
        }
        for (size_t slot = 0u; slot < KNVR_TRACK_MAX; slot++) {
            int gap;

            if (!tracker->used[slot] || matched[slot] ||
                boxes[i].class_id != tracker->tracks[slot].class_id ||
                !near_enough(&tracker->tracks[slot].box, &boxes[i])) {
                continue;
            }
            gap = distance(centre(&tracker->tracks[slot].box),
                           centre(&boxes[i]));
            if (best_slot == KNVR_TRACK_MAX || gap < best_gap) {
                best_slot = slot;
                best_gap = gap;
            }
        }
        if (best_slot != KNVR_TRACK_MAX) {
            matched[best_slot] = true;
            taken[i] = true;
            tracker->assigned[i] = tracker->tracks[best_slot].id;
        }
    }

    /* Apply the matches. */
    for (size_t i = 0u; i < count; i++) {
        if (tracker->assigned[i] == 0) {
            continue;
        }
        for (size_t slot = 0u; slot < KNVR_TRACK_MAX; slot++) {
            knvr_track *track = &tracker->tracks[slot];

            if (!tracker->used[slot] || track->id != tracker->assigned[i]) {
                continue;
            }
            track->box = boxes[i];
            if (boxes[i].score > track->score) {
                track->score = boxes[i].score;
            }
            track->last_seen = at_ms;
            track->hits++;
            track->misses = 0;
            if (track->hits >= tracker->options.min_hits) {
                track->confirmed = true;
            }
            remember(tracker, slot, &boxes[i], at_ms);
            break;
        }
    }

    /* Everything else is new. */
    for (size_t i = 0u; i < count; i++) {
        if (!taken[i]) {
            (void)spawn(tracker, &boxes[i], at_ms, &tracker->assigned[i]);
        }
    }

    for (size_t slot = 0u; slot < KNVR_TRACK_MAX; slot++) {
        if (tracker->used[slot] && !matched[slot] &&
            tracker->tracks[slot].last_seen != at_ms) {
            tracker->tracks[slot].misses++;
        }
    }
    return true;
}

size_t knvr_tracker_count(const knvr_tracker *tracker)
{
    size_t live = 0u;

    if (tracker == NULL) {
        return 0u;
    }
    for (size_t slot = 0u; slot < KNVR_TRACK_MAX; slot++) {
        if (tracker->used[slot]) {
            live++;
        }
    }
    return live;
}

const knvr_track *knvr_tracker_at(const knvr_tracker *tracker, size_t index)
{
    size_t seen = 0u;

    if (tracker == NULL) {
        return NULL;
    }
    for (size_t slot = 0u; slot < KNVR_TRACK_MAX; slot++) {
        if (!tracker->used[slot]) {
            continue;
        }
        if (seen == index) {
            return &tracker->tracks[slot];
        }
        seen++;
    }
    return NULL;
}

int64_t knvr_tracker_assigned(
    const knvr_tracker *tracker, size_t detection_index)
{
    if (tracker == NULL || detection_index >= tracker->assigned_count) {
        return 0;
    }
    return tracker->assigned[detection_index];
}

int64_t knvr_tracker_total(const knvr_tracker *tracker)
{
    return tracker == NULL ? 0 : tracker->total;
}

/* ------------------------------- drawing -------------------------------- */

static const uint32_t TRACK_COLOURS[] = {
    0x40FF40u, 0xFFB020u, 0x50B0FFu, 0xFF60C0u,
    0xFFFF50u, 0x80FFE0u, 0xC080FFu, 0xFF8060u
};

static void put(uint8_t *rgba, int width, int height, int x, int y,
                uint32_t colour, bool dim)
{
    uint8_t *pixel;
    uint8_t red = (uint8_t)((colour >> 16) & 0xFFu);
    uint8_t green = (uint8_t)((colour >> 8) & 0xFFu);
    uint8_t blue = (uint8_t)(colour & 0xFFu);

    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    if (dim) {
        red = (uint8_t)(red / 3u);
        green = (uint8_t)(green / 3u);
        blue = (uint8_t)(blue / 3u);
    }
    pixel = rgba + ((size_t)y * (size_t)width + (size_t)x) * 4u;
    pixel[0] = blue;
    pixel[1] = green;
    pixel[2] = red;
}

void knvr_track_draw(
    uint8_t *rgba, int width, int height, const knvr_tracker *tracker)
{
    const size_t palette = sizeof(TRACK_COLOURS) / sizeof(TRACK_COLOURS[0]);

    if (rgba == NULL || tracker == NULL || width <= 0 || height <= 0) {
        return;
    }
    for (size_t index = 0u;; index++) {
        const knvr_track *track = knvr_tracker_at(tracker, index);
        uint32_t colour;
        int x1;
        int y1;

        if (track == NULL) {
            break;
        }
        if (!track->confirmed) {
            continue;
        }
        colour = TRACK_COLOURS[(size_t)track->id % palette];
        x1 = track->box.x + track->box.w - 1;
        y1 = track->box.y + track->box.h - 1;
        for (int x = track->box.x; x <= x1; x++) {
            put(rgba, width, height, x, track->box.y, colour,
                track->stationary);
            put(rgba, width, height, x, y1, colour, track->stationary);
        }
        for (int y = track->box.y; y <= y1; y++) {
            put(rgba, width, height, track->box.x, y, colour,
                track->stationary);
            put(rgba, width, height, x1, y, colour, track->stationary);
        }
        /* The trail, so a still frame shows which way it was going. */
        for (size_t step = 0u; step < track->path_length; step++) {
            const knvr_point point = track->path[step];

            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    put(rgba, width, height, point.x + dx, point.y + dy,
                        colour, true);
                }
            }
        }
    }
}
