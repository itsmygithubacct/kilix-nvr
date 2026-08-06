# kilix-nvr

A network video recorder that lives in a terminal.

`kilix-nvr` records RTSP cameras, detects people and animals in what they see,
and lets you review what happened — without a browser anywhere in it. Live
views, playback and the review interface all render through the [Kitty graphics
protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/), using
[`kilix-rtsp`](https://github.com/itsmygithubacct/kilix-rtsp) for acquisition
and presentation.

**Status: design complete, implementation not started.** Nothing here builds
yet. The sections below describe what is being built and why it is shaped this
way; they are not a description of working software.

## Design

**The model runs outside the core.** Object detection is a supervised
subprocess speaking a fixed-size protocol on stdin/stdout — a tensor in, a
`float32[20][6]` detection array out. Nothing links an ML runtime. That keeps
inference restartable and isolated, lets the detector run on a different
machine than the recorder without changing anything but the command, and means
the model can be replaced without touching the recorder.

**Detection is gated behind motion.** Motion detection is cheap arithmetic on a
downscaled luma frame; inference is milliseconds per region. Running the second
only where the first found something is what makes a multi-camera recorder fit
on a small machine.

**One `ffmpeg` per camera, with two outputs** — a raw pipe for detection and
display, and a `-c copy` segmenter for storage. Many network cameras refuse a
second concurrent RTSP session, so opening one connection per job is not an
option. Recording never re-encodes: at the same resolution, re-encoding
reliably produces *larger* files than the camera's own stream.

**The filesystem is authoritative.** Recordings are named by timestamp on disk
and SQLite is an index over them, rebuildable by walking the tree. A recorder
whose database and storage can disagree needs a reconciliation job that stays
expensive forever.

## Planned commands

```sh
kilix-nvr record    [camera...]     # capture and segment
kilix-nvr detect    [camera...]     # motion and object detection
kilix-nvr events    [--since ...]   # query what happened
kilix-nvr review                    # review interface
kilix-nvr play      <event|time>    # playback
kilix-nvr reanalyze <event>         # re-run detection over stored footage
kilix-nvr prune                     # apply retention
```

## Dependencies

A C11 compiler, POSIX, pthreads, and `sqlite3` at link time. At runtime: the
`ffmpeg` binary and a detector command. No ML library is linked.

## Configuration and data

Everything the program reads or writes lives outside this repository, under
`~/.local/gpu_terminal/kilix-nvr/` — configuration, recordings, the event
index, state and logs. Override the root with `KILIX_NVR_HOME`.

**Camera configuration is a secret file.** RTSP URLs embed credentials as
`rtsp://user:password@host/path`, so it lives outside the work tree, is refused
unless owned by you with no group or world permission bits, and is redacted
everywhere it would otherwise be shown — logs, event records, error messages
and the detector handoff included.

## License

MIT. See `LICENSE`.
