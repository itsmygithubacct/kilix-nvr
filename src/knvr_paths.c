#include "knvr_paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static bool ensure_directory(const char *path)
{
    struct stat info;

    if (stat(path, &info) == 0) {
        return S_ISDIR(info.st_mode);
    }
    /* 0700 throughout.  The event store records when a house was empty,
     * which is not a fact to leave group-readable by default. */
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

bool knvr_paths_home(char *out, size_t size)
{
    const char *override = getenv("KILIX_NVR_HOME");
    const char *home;
    int written;

    if (out == NULL || size == 0u) {
        return false;
    }
    if (override != NULL && override[0] == '/') {
        written = snprintf(out, size, "%s", override);
    } else {
        home = getenv("HOME");
        if (home == NULL || home[0] != '/') {
            return false;
        }
        written = snprintf(out, size, "%s/.local/gpu_terminal/kilix-nvr",
                           home);
    }
    if (written < 0 || (size_t)written >= size) {
        return false;
    }
    return ensure_directory(out);
}

bool knvr_paths_subdir(char *out, size_t size, const char *leaf)
{
    char home[KNVR_PATH_MAX];
    int written;

    if (out == NULL || leaf == NULL || strchr(leaf, '/') != NULL) {
        return false;
    }
    if (!knvr_paths_home(home, sizeof(home))) {
        return false;
    }
    written = snprintf(out, size, "%s/%s", home, leaf);
    if (written < 0 || (size_t)written >= size) {
        return false;
    }
    return ensure_directory(out);
}

bool knvr_paths_state_file(char *out, size_t size, const char *leaf)
{
    char directory[KNVR_PATH_MAX];
    int written;

    if (out == NULL || leaf == NULL || strchr(leaf, '/') != NULL) {
        return false;
    }
    if (!knvr_paths_subdir(directory, sizeof(directory), "state")) {
        return false;
    }
    written = snprintf(out, size, "%s/%s", directory, leaf);
    return written >= 0 && (size_t)written < size;
}
