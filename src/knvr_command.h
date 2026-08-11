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

/*
 * Where a bundled tool is, preferring one that travels with this binary.
 *
 * `make install` puts the tools beside the command, but the catalog
 * installs a checkout and runs `build/kilix-nvr` in place, where nothing
 * is on PATH at all.  Looking next to the executable first, then in the
 * checkout's tools/, then falling back to a bare name for PATH, means the
 * detector is found however the program was installed - and "no detector;
 * motion only" then means the model is missing rather than the script.
 *
 * Always writes something: the bare name when nothing better is found.
 */
void knvr_command_bundled(const char *name, char *out, size_t size);

#endif /* KNVR_COMMAND_H */
