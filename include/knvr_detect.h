#ifndef KNVR_DETECT_H
#define KNVR_DETECT_H

/*
 * The object detector, as a supervised subprocess.
 *
 * The whole contract is 480 bytes.  Every detector plugin the reference
 * implementations ship - EdgeTPU, OpenVINO, TensorRT, RKNN, Hailo and the
 * rest - produces exactly float32[20][6], rows of
 * [class, score, y0, x0, y1, x1] with coordinates normalised 0-1.  That is
 * small enough to be a pipe protocol rather than an RPC.
 *
 * So: write exactly one tensor, read exactly 480 bytes.  A fixed-size
 * request paired with a fixed-size reply is self-framing, which means no
 * parser, no delimiters and no library - the same pattern kilix-rtsp
 * already uses for ffmpeg, supervised by the same kind of watchdog.
 *
 * Nothing here links an ML runtime, and nothing here knows about an
 * accelerator.  Both live behind the command, which is why *where*
 * inference happens - this machine, a GPU box, a USB stick - is a launch
 * detail rather than an architectural one.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNVR_DETECT_ROWS 20
#define KNVR_DETECT_COLUMNS 6
#define KNVR_DETECT_BYTES \
    (KNVR_DETECT_ROWS * KNVR_DETECT_COLUMNS * (int)sizeof(float))
#define KNVR_DETECT_LABEL_MAX 32

typedef struct knvr_detection_box {
    int class_id;
    float score;
    /* Frame pixels, converted from the normalised reply once here so no
     * consumer has to know the tensor was normalised. */
    int x;
    int y;
    int w;
    int h;
} knvr_detection_box;

typedef struct knvr_detector knvr_detector;

typedef struct knvr_detector_options {
    /* The command, argv-style, NULL-terminated.  NULL uses the bundled
     * detector on PATH. */
    const char *const *argv;

    /* Frame geometry the detector is fed.  Fixed for the life of the
     * process, because that is what makes the pipe self-framing. */
    int width;
    int height;

    /*
     * Below this, a row is dropped.  Default 0.25 - measured, and lower
     * than it looks: the class allowlist is what filters nonsense, not
     * the threshold, and 0.45 was demonstrably wrong because it dropped
     * real people in infrared.
     */
    float min_score;

    /* Seconds to wait for a reply before deciding the detector has
     * wedged.  Default 5. */
    int timeout_seconds;
} knvr_detector_options;

void knvr_detector_options_init(knvr_detector_options *options);

bool knvr_detector_start(
    knvr_detector **detector, const knvr_detector_options *options);
void knvr_detector_stop(knvr_detector *detector);
const char *knvr_detector_error(const knvr_detector *detector);

/*
 * One frame in, boxes out.
 *
 * `frame` is width * height BGRA pixels.  Returns false when the detector
 * could not be reached, which the caller should treat as "this camera is
 * motion-only for now" rather than as fatal: a dead detector must not
 * take the pipeline down with it.
 */
bool knvr_detector_run(
    knvr_detector *detector, const uint8_t *frame,
    knvr_detection_box *boxes, size_t capacity, size_t *count);

/*
 * The COCO label for a class id, or NULL when the id is outside the
 * allowlist.
 *
 * An allowlist rather than the full 80 classes, and that is the real
 * defence against nonsense: across 140 frames of still footage the models
 * invented toilets, birds and aeroplanes, none of which this is asked to
 * report.  Filtering by name is what makes a low confidence threshold
 * safe.
 */
const char *knvr_detect_label(int class_id);

#ifdef __cplusplus
}
#endif

#endif /* KNVR_DETECT_H */
