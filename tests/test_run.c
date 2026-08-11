/*
 * The runner's contract with a viewer.
 *
 * Not the pipeline - that needs cameras - but the part two programs have
 * to agree on: what a camera's ring is called, and that a second process
 * can attach to it and read the frames the recorder decoded.  This is
 * tested across a real fork rather than in one process, because "it works
 * in-process" is exactly the claim shared memory can satisfy while still
 * being useless to a viewer.
 */

#include "knvr_run.h"

#include "kilix_rtsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n",                \
                          __FILE__, __LINE__, #condition);                    \
            return false;                                                     \
        }                                                                     \
    } while (false)

#define WIDTH 64
#define HEIGHT 32

static bool test_the_name_is_composed_not_guessed(void)
{
    char name[128];

    CHECK(knvr_run_ring_name("drivecam", name, sizeof(name)));
    CHECK(strcmp(name, "kilix-nvr.drivecam") == 0);
    /* Both sides call this, so a viewer looking for a camera it was told
     * about finds the ring the recorder made. */
    CHECK(!knvr_run_ring_name(NULL, name, sizeof(name)));
    CHECK(!knvr_run_ring_name("drivecam", NULL, sizeof(name)));
    return true;
}

static bool test_options_publish_by_default(void)
{
    knvr_run_options options;

    /* A recorder nobody can watch is the wrong default: the viewer's
     * whole reason to prefer an attached ring is that it costs the
     * camera nothing. */
    (void)memset(&options, 0xff, sizeof(options));
    knvr_run_options_init(&options);
    CHECK(options.publish);
    CHECK(options.cameras == NULL);
    CHECK(options.count == 0u);
    CHECK(options.seconds == 0);
    CHECK(!options.verbose);
    CHECK(options.render == NULL);
    return true;
}

/* A frame written by one process is the frame another one reads. */
static bool test_another_process_reads_the_frames(void)
{
    char name[128];
    krtsp_frame *ring = NULL;
    uint8_t *back;
    size_t size;
    pid_t child;
    int status = 0;

    /* Named for a camera that does not exist, so a live recorder's ring
     * is never what this test attaches to. */
    CHECK(knvr_run_ring_name("test-run-ring", name, sizeof(name)));
    CHECK(krtsp_frame_init_shared(&ring, name, WIDTH, HEIGHT, 2));

    size = krtsp_frame_size(ring);
    CHECK(size == (size_t)WIDTH * (size_t)HEIGHT * 4u);
    back = krtsp_frame_back(ring);
    CHECK(back != NULL);
    (void)memset(back, 0x5a, size);
    krtsp_frame_publish(ring, NULL);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        krtsp_frame *attached = NULL;
        const uint8_t *frame = NULL;
        uint64_t sequence = 0u;
        int found = 1;

        if (!krtsp_frame_attach(&attached, name)) {
            _exit(2);
        }
        /* The producer published before the fork, so the newest frame is
         * already there; a borrow that returns nothing here would mean
         * the ring only works for a reader that was watching. */
        frame = krtsp_frame_borrow(attached, &sequence, NULL);
        if (frame != NULL && frame[0] == 0x5a &&
            frame[krtsp_frame_size(attached) - 1u] == 0x5a) {
            found = 0;
        }
        if (frame != NULL) {
            krtsp_frame_release(attached);
        }
        krtsp_frame_free(attached);
        _exit(found);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);

    krtsp_frame_free(ring);

    /* The producer owns the object: once it is gone, attaching to a
     * camera that stopped recording must fail rather than hand a viewer
     * a frame frozen at whatever was last published. */
    {
        krtsp_frame *stale = NULL;

        CHECK(!krtsp_frame_attach(&stale, name));
        krtsp_frame_free(stale);
    }
    return true;
}

int main(void)
{
    const struct {
        const char *name;
        bool (*function)(void);
    } tests[] = {
        {"the ring name is composed", test_the_name_is_composed_not_guessed},
        {"a run publishes by default", test_options_publish_by_default},
        {"another process reads the frames",
         test_another_process_reads_the_frames}
    };
    size_t passed = 0u;

    for (size_t index = 0u; index < sizeof(tests) / sizeof(tests[0]); ++index) {
        const bool ok = tests[index].function();

        (void)printf("%s %s\n", ok ? "ok" : "not ok", tests[index].name);
        if (!ok) {
            return 1;
        }
        ++passed;
    }
    (void)printf("%zu tests passed\n", passed);
    return 0;
}
