#ifndef KNVR_PATHS_H
#define KNVR_PATHS_H

/*
 * Where this program's files live.
 *
 * Outside the repository, under the same ~/.local/gpu_terminal root the
 * rest of the stack uses, overridable with KILIX_NVR_HOME so a test never
 * touches live data.  Nothing here is committed and nothing here is
 * shared with kilix-rtsp's own directory: cameras.conf is its file, and a
 * program that writes into another's state directory is a program that
 * breaks when the other one tidies up.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNVR_PATH_MAX 1024

/* The root, honouring KILIX_NVR_HOME. */
bool knvr_paths_home(char *out, size_t size);

/*
 * A file under <home>/state, creating the directory if needed.  Mode 0700
 * throughout: the event store records when a house was empty.
 */
bool knvr_paths_state_file(char *out, size_t size, const char *leaf);

/* A directory under <home>, created if needed. */
bool knvr_paths_subdir(char *out, size_t size, const char *leaf);

#ifdef __cplusplus
}
#endif

#endif /* KNVR_PATHS_H */
