#ifndef KNVR_STRIP_H
#define KNVR_STRIP_H

/*
 * A scalar over time, drawn as a waveform.
 *
 * Motion and sound are the same shape of thing: one number a second,
 * between nothing and everything.  Drawn as a strip under the picture
 * they answer what neither a box nor a label can - a bark with nothing
 * moving is a different fact from a bark with a shape at the gate, and
 * two aligned strips make that one glance.
 *
 * Frigate draws its review timeline this way already (a two-sided bar per
 * segment, motion and audio interpolated identically).  What is different
 * here is position: this is not a decoration on a scrubber, it is the
 * instrument you read the camera by, and it appears under the live view
 * as well as under the day.
 */

#include "soft_raster.h"

#include <stdbool.h>
#include <stddef.h>

/* A mark on the strip: where something was recorded, in the same x space
 * as the samples. */
typedef struct knvr_strip_mark {
    size_t at;          /* sample index */
    uint32_t colour;
} knvr_strip_mark;

typedef struct knvr_strip {
    const char *label;
    const float *samples;   /* 0..1 each; NULL draws an empty track */
    size_t count;
    uint32_t colour;
    /* Drawn as a dashed line across the strip, or negative for none: a
     * threshold you cannot see is a number you cannot choose. */
    float threshold;
    /* Where "now" is, or negative for none.  Fractional so a scrubber can
     * sit between samples. */
    float cursor;
    const knvr_strip_mark *marks;
    size_t mark_count;
} knvr_strip;

/*
 * Draw one strip into `canvas` at (x, y), `width` by `height`.
 *
 * Mirrored about the centre line, because a two-sided wave reads as a
 * signal where a bar chart reads as a table.  Samples are taken by
 * peak when there are more of them than pixels: the loudest thing in a
 * column is what the column should say.
 */
void knvr_strip_draw(
    sr_canvas *canvas, const knvr_strip *strip, int x, int y, int width,
    int height);

/*
 * A ring of the last `capacity` samples, for the live view.
 *
 * The live strips scroll; keeping them in a ring rather than re-reading
 * the store every frame is the difference between a redraw and a query.
 */
typedef struct knvr_ring {
    float *values;
    size_t capacity;
    size_t count;
    size_t next;
} knvr_ring;

bool knvr_ring_init(knvr_ring *ring, size_t capacity);
void knvr_ring_free(knvr_ring *ring);
void knvr_ring_push(knvr_ring *ring, float value);
/*
 * Copy the ring out oldest-first into `out`, padding the front with zero
 * when it is not full yet - so a strip is the same width from the first
 * second and simply fills up from the right.
 */
void knvr_ring_read(const knvr_ring *ring, float *out, size_t count);

#endif /* KNVR_STRIP_H */
