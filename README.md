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

**One RTSP session and one decode per camera, ever.** Decoded frames are
published into a shared-memory ring, and everything that wants pixels — motion
detection, the object detector, the live view, the recorder — reads the same
frames out of it. Nothing else connects to the camera and nothing else decodes.
Decoding is the most expensive thing in the system, and many network cameras
refuse a second concurrent session anyway, so a design that needs only one works
on cameras a design that needs two does not.

**The model runs outside the core.** Object detection is a supervised subprocess
speaking a fixed-size protocol on stdin/stdout — a tensor in, a
`float32[20][6]` detection array out. Nothing links an ML runtime. That keeps
inference restartable and isolated, lets the model be replaced without touching
the recorder, and makes *where* it runs a matter of which command is configured:
on the recorder, on the machine doing the rendering, or over `ssh` to whatever
box has a GPU.

**Detection is optional per camera, and gated behind motion.** Motion detection
is cheap arithmetic on the luma plane of a frame that is already in memory;
inference is milliseconds. Running the second only when the first finds
something bounds the cost by how much actually happens rather than by camera
count times frame rate — and a camera that does not need detection simply does
not have a detector attached.

**Recording is a per-camera mode.** `off`, `stills` on detection, `clips`
around each event, or `continuous` — byte-exact capture of everything. The first
three work off the shared ring; `continuous` adds a second `-c copy` output sink
to the same ffmpeg process, taking the camera's own packets to disk without ever
decoding them, at near-zero CPU. It is never a re-encode: at the same
resolution, re-encoding reliably produces *larger* files than the camera's own
stream.

The modes are independent per camera, and the combinations are all real — a
camera set to `continuous` with detection off and nobody watching does not
decode at all, costing only I/O. In `continuous` mode pre-roll also stops being
a feature and becomes a query: the seconds before a trigger are already on disk,
so an event simply names an earlier start time. `stills` is the default, because
it answers the usual requirement three or four orders of magnitude more cheaply.

**The filesystem is authoritative.** Events are named by camera and timestamp on
disk, and SQLite is an index over them, rebuildable by walking the tree. A
recorder whose database and storage can disagree needs a reconciliation job that
stays expensive forever.

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
