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
    if (mkdir(path, 0700) == 0) {
        return true;
    }
    return errno == EEXIST && stat(path, &info) == 0 &&
           S_ISDIR(info.st_mode);
}

bool knvr_paths_mkdirs(const char *path)
{
    char copy[KNVR_PATH_MAX];
    size_t length;

    if (path == NULL || path[0] != '/') {
        errno = EINVAL;
        return false;
    }
    length = strlen(path);
    if (length >= sizeof(copy)) {
        errno = ENAMETOOLONG;
        return false;
    }
    (void)memcpy(copy, path, length + 1u);
    for (size_t i = 1u; i <= length; i++) {
        if (copy[i] == '/' || copy[i] == '\0') {
            const char saved = copy[i];
            copy[i] = '\0';
            if (!ensure_directory(copy)) {
                return false;
            }
            copy[i] = saved;
        }
    }
    return true;
}

bool knvr_paths_home(char *out, size_t size)
{
    const char *override = getenv("KILIX_NVR_HOME");
    const char *home;
    int written;

    if (out == NULL || size == 0u) {
        return false;
    }
    if (override != NULL && override[0] != '\0') {
        if (override[0] != '/') {
            errno = EINVAL;
            return false;
        }
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
    return knvr_paths_mkdirs(out);
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
