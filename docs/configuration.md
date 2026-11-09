# Dependencies, configuration and data

What the recorder needs at build and run time, where inference runs, where its data lives and how camera secrets are kept. Moved from the README.

## Dependencies

A C11 compiler, POSIX, pthreads, and `sqlite3` at link time. At runtime: the
`ffmpeg` binary and a detector command. No ML library is linked, and no
accelerator runtime — those live behind the detector command, wherever it runs.

**Where inference runs is an environment variable**, since it is a property of
the host rather than of a camera:

```sh
KILIX_OBJECT_DETECTOR="ssh gpubox kilix-look-detect"
KILIX_SOUND_CLASSIFIER="$HOME/.venvs/audio/bin/python \
                        /usr/local/bin/kilix-listen-classify"
```

Unset, each falls back to the bundled tool — beside the binary if `make
install` put it there, else in the checkout's `tools/`, else on `PATH`. The
bundled object helper runs `kilix yolox run`: it checks the selected installed
YOLOX model, agreement and runtime before starting inference. Prepare that
runtime with `kilix install yolox`; recording never installs a model or
answers its agreement prompt. Missing or invalid runtime state reports a
detector failure while motion recording remains available. An explicit
`KILIX_OBJECT_DETECTOR` continues to select your own command. Both overrides
are split on spaces with no quoting: a path containing a space needs a wrapper
script, which is a smaller surprise than half-implemented shell quoting.

## Configuration and data

Recorder policy, recordings, the event index, state and logs live outside this
repository under `~/.local/gpu_terminal/kilix-nvr/`. Override that root with
`KILIX_NVR_HOME`, using an absolute path. Missing parent directories are
created privately; a relative override is refused instead of silently opening
the default store. Camera names and URLs are shared with `kilix-rtsp` through
`~/.local/gpu_terminal/kilix-rtsp/config/cameras.conf` (root override:
`KILIX_RTSP_HOME`). Define the URL there before adding NVR policy for its name.
For a private local relay, a minimal camera definition is:

```ini
[camera "doorway"]
main = rtsp://127.0.0.1:18554/doorway
sub = rtsp://127.0.0.1:18554/doorway
```

The example requires an existing relay at that address. Keep the file mode
`0600` and its component directories `0700`; then run `kilix-nvr add doorway`,
`kilix-nvr check doorway`, and `kilix-nvr view doorway`. Adding policy leaves
recording and detection disabled until explicitly configured.

**Camera configuration is a secret file.** RTSP URLs embed credentials as
`rtsp://user:password@host/path`, so it lives outside the work tree, is refused
unless owned by you with no group or world permission bits, and is redacted
everywhere it would otherwise be shown — logs, event records, error messages
and the detector handoff included.
