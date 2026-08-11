#ifndef KNVR_DETECT_H
#define KNVR_DETECT_H

/*
 * The detector, which now lives in kilix-object-detect.
 *
 * What is left here is the recorder's own shape for a detection - flat
 * coordinates, because the tracker and the zones were written against
 * them and a nested rectangle would be a rename with no reader benefit -
 * and the one conversion between that and the module's boxes.
 *
 * The module is where the interesting part went: detection runs on crops
 * around what moved rather than on whole frames, which is both cheaper
 * and better at small subjects, and which means a preclusive zone now
 * costs no inference at all instead of being filtered after the fact.
 */

#include "kilix_object_detect.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kept as the recorder's own name for the contract's row count. */
#define KNVR_DETECT_ROWS KOD_BOX_MAX
#define KNVR_DETECT_LABEL_MAX 32

typedef struct knvr_detection_box {
    int class_id;
    float score;
    int x;
    int y;
    int w;
    int h;
} knvr_detection_box;

/* The COCO label for a class id, or NULL outside the allowlist. */
static inline const char *knvr_detect_label(int class_id)
{
    return kod_label(class_id);
}

/* One of the module's boxes as one of ours. */
static inline knvr_detection_box knvr_detect_from(const kod_box *box)
{
    knvr_detection_box out;

    out.class_id = box->class_id;
    out.score = box->score;
    out.x = box->at.x;
    out.y = box->at.y;
    out.w = box->at.w;
    out.h = box->at.h;
    return out;
}

#ifdef __cplusplus
}
#endif

#endif /* KNVR_DETECT_H */
