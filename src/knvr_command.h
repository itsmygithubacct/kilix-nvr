#ifndef KNVR_COMMAND_H
#define KNVR_COMMAND_H

/*
 * Where inference runs, as an environment variable.
 *
 * The detector and the listener are subprocesses precisely so that
 * *where* they run is not an architectural decision - this machine, a
 * venv with a runtime in it, a GPU box over ssh.  That promise needs a
 * way to say which, and it is not a per-camera policy: every camera on a
 * host reaches the same models the same way.
 *
 *   KILIX_NVR_DETECT="ssh gpubox kilix-nvr-detect"
 *   KILIX_NVR_LISTEN="/home/me/.local/share/kilix-nvr/venv/bin/python \
 *                     /usr/local/bin/kilix-nvr-listen"
 *
 * Split on spaces, with no quoting: a path with a space in it needs a
 * wrapper script, which is a smaller surprise than half-implemented shell
 * quoting that works until it does not.
 */

#include <stdbool.h>
#include <stddef.h>

/*
 * Fill `argv` from `variable`, pointing into `storage` (a scratch buffer
 * this chops up).  Returns false when the variable is unset, empty, or
 * has more words than `capacity - 1`, in which case the caller uses its
 * own default.  `argv` is always NULL-terminated on success.
 */
bool knvr_command_from_env(
    const char *variable, char *storage, size_t storage_size,
    const char **argv, size_t capacity);

#endif /* KNVR_COMMAND_H */
