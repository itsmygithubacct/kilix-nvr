#include "knvr_strip.h"

#include <stdlib.h>
#include <string.h>

#define TRACK 0x00202028u
#define LINE 0x00FF6060u
#define DIM 0x00707884u

void knvr_strip_draw(
    sr_canvas *canvas, const knvr_strip *strip, int x, int y, int width,
    int height)
{
    const int label_width = 64;
    const int left = x + label_width;
    const int plot = width - label_width;
    const int middle = y + height / 2;

    if (canvas == NULL || strip == NULL || plot <= 0 || height < 6) {
        return;
    }
    if (strip->label != NULL) {
        sr_text(canvas, (float)x, (float)(middle - 6), strip->label, DIM,
                1.0f, 1);
    }
    sr_fill_rect(canvas, (float)left, (float)y, (float)plot, (float)height,
                 TRACK, 1.0f);
    sr_line(canvas, (float)left, (float)middle, (float)(left + plot),
            (float)middle, 1.0f, 0x00303040u, 1.0f, 0, 0);
    if (strip->samples == NULL || strip->count == 0u) {
        return;
    }
    for (int column = 0; column < plot; column++) {
        const size_t from = (size_t)column * strip->count / (size_t)plot;
        size_t to = (size_t)(column + 1) * strip->count / (size_t)plot;
        float peak = 0.0f;
        int reach;

        if (to <= from) {
            to = from + 1u;
        }
        /* Peak, not mean: with more samples than pixels the loudest thing
         * in the column is what the column should say, and averaging is
         * how a bark in a quiet minute disappears. */
        for (size_t i = from; i < to && i < strip->count; i++) {
            if (strip->samples[i] > peak) {
                peak = strip->samples[i];
            }
        }
        if (peak <= 0.0f) {
            continue;
        }
        if (peak > 1.0f) {
            peak = 1.0f;
        }
        reach = (int)(peak * (float)(height / 2 - 1));
        if (reach < 1) {
            reach = 1;
        }
        sr_line(canvas, (float)(left + column), (float)(middle - reach),
                (float)(left + column), (float)(middle + reach), 1.0f,
                strip->colour, 1.0f, 0, 0);
    }
    for (size_t i = 0u; i < strip->mark_count; i++) {
        const float at = (float)strip->marks[i].at / (float)strip->count;
        const float column = (float)left + at * (float)plot;

        sr_line(canvas, column, (float)y, column, (float)(y + height), 1.0f,
                strip->marks[i].colour, 0.8f, 0, 0);
    }
    if (strip->threshold >= 0.0f && strip->threshold <= 1.0f) {
        const int reach = (int)(strip->threshold * (float)(height / 2 - 1));

        sr_line(canvas, (float)left, (float)(middle - reach),
                (float)(left + plot), (float)(middle - reach), 1.0f, LINE,
                0.55f, 4, 4);
        sr_line(canvas, (float)left, (float)(middle + reach),
                (float)(left + plot), (float)(middle + reach), 1.0f, LINE,
                0.55f, 4, 4);
    }
    if (strip->cursor >= 0.0f) {
        const float column =
            (float)left + (strip->cursor / (float)strip->count) *
                              (float)plot;

        sr_line(canvas, column, (float)(y - 2), column,
                (float)(y + height + 2), 1.0f, 0x00FFFFFFu, 0.9f, 0, 0);
    }
}

/* ---------------------------------- ring --------------------------------- */

bool knvr_ring_init(knvr_ring *ring, size_t capacity)
{
    if (ring == NULL || capacity == 0u) {
        return false;
    }
    ring->values = calloc(capacity, sizeof(*ring->values));
    if (ring->values == NULL) {
        return false;
    }
    ring->capacity = capacity;
    ring->count = 0u;
    ring->next = 0u;
    return true;
}

void knvr_ring_free(knvr_ring *ring)
{
    if (ring == NULL) {
        return;
    }
    free(ring->values);
    ring->values = NULL;
    ring->capacity = 0u;
    ring->count = 0u;
    ring->next = 0u;
}

void knvr_ring_push(knvr_ring *ring, float value)
{
    if (ring == NULL || ring->values == NULL) {
        return;
    }
    ring->values[ring->next] = value;
    ring->next = (ring->next + 1u) % ring->capacity;
    if (ring->count < ring->capacity) {
        ring->count++;
    }
}

void knvr_ring_read(const knvr_ring *ring, float *out, size_t count)
{
    if (out == NULL || count == 0u) {
        return;
    }
    for (size_t i = 0u; i < count; i++) {
        out[i] = 0.0f;
    }
    if (ring == NULL || ring->values == NULL || ring->count == 0u) {
        return;
    }
    /* Oldest first, and short histories fill from the right so a strip is
     * the same width from the first second. */
    for (size_t i = 0u; i < ring->count && i < count; i++) {
        const size_t from =
            (ring->next + ring->capacity - ring->count + i) % ring->capacity;

        out[count - (ring->count < count ? ring->count : count) + i] =
            ring->values[from];
    }
}
