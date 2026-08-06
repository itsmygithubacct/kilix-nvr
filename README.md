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

## What you configure

Every camera is a set of independently selectable capabilities. You choose them
when you add the camera, and you can change any of them afterwards:

| capability | values | on a new camera |
| --- | --- | --- |
| record | `none` / `stills` / `clips` / `continuous` | **`none`** |
| pre-roll | seconds, `continuous` only | 10 s |
| motion detection | on / off | off |
| object detection | `off` / `always` / `on-view` | off |
| object classes | any COCO labels | person + animals |
| audio record | on / off | off |
| audio detection | on / off | off |
| decode size | W×H | camera's substream |

**A newly added camera is viewable and records nothing.** Nothing starts
consuming disk without being asked for.

Object detection implies motion detection, which is its gate. Motion detection
on its own is a complete configuration — events for movement, without ever
asking a model what caused it.

## Design

**One RTSP session and one decode per camera, ever.** Decoded frames are
published into a shared-memory ring, and everything that wants pixels — motion
detection, the object detector, the live view, the recorder — reads the same
frames out of it. Nothing else connects to the camera and nothing else decodes.

The reason is cost, not capability: decoding is the most expensive thing in the
system, and a second decode produces bytes that are already in memory. Cameras
generally do tolerate concurrent sessions — two sustained readers on one camera
were verified running for 14 hours — so `kilix-nvr` can coexist with another
recorder. It simply never needs to.

**The model runs outside the core.** Object detection is a supervised subprocess
speaking a fixed-size protocol on stdin/stdout — a tensor in, a
`float32[20][6]` detection array out. Nothing links an ML runtime. That keeps
inference restartable and isolated, lets the model be replaced without touching
the recorder, and makes *where* it runs a matter of which command is configured:
on the recorder, on the machine doing the rendering, or over `ssh` to whatever
box has a GPU.

**It runs on more than one kind of machine.** Commodity x86, a box with a GPU,
or a small ARM board near the cameras. The core links no accelerator anything —
no CUDA, no NPU runtime, no ONNX, no OpenCV — so every hardware dependency lives
on the far side of the detector pipe. Accelerators are detected and optional,
never assumed, and probed by running them rather than by reading a capability
list, because a listed accelerator is not a working one.

**Detection is gated behind motion.** Motion detection is cheap arithmetic on
the luma plane of a frame that is already in memory; inference is milliseconds.
Running the second only when the first finds something bounds the cost by how
much actually happens rather than by camera count times frame rate.

**Recording never re-encodes.** `continuous` adds a second `-c copy` output sink
to the same ffmpeg process, taking the camera's own packets to disk without ever
decoding them, at near-zero CPU — at the same resolution, re-encoding reliably
produces *larger* files than the source. This is the one thing the shared ring
cannot provide, because decoded pixels cannot be turned back into packets.

Two things fall out of the sinks being independent. A camera on `continuous`
with detection off and nobody watching **never decodes at all**, costing only
I/O. And pre-roll stops being a feature and becomes a query: the seconds before
a trigger are already on disk, so an event simply names an earlier start time.

**The filesystem is authoritative.** Events are named by camera and timestamp on
disk, and SQLite is an index over them, rebuildable by walking the tree. A
recorder whose database and storage can disagree needs a reconciliation job that
stays expensive forever.

## Planned commands

```sh
kilix-nvr add       <name> <url>    # onboard a camera; prompts for capabilities
kilix-nvr set       <name> <k=v>... # change any capability afterwards
kilix-nvr cameras                   # what exists and what each one is doing
kilix-nvr run       [camera...]     # the pipeline, honouring each camera's config
kilix-nvr events    [--since ...]   # query what happened
kilix-nvr review                    # review interface
kilix-nvr play      <event|time>    # playback
kilix-nvr reanalyze <event>         # re-run detection over stored footage
kilix-nvr prune                     # apply retention
```

There is no separate `record` or `detect` command: what a camera does is its
configuration, not an invocation.

## Dependencies

A C11 compiler, POSIX, pthreads, and `sqlite3` at link time. At runtime: the
`ffmpeg` binary and a detector command. No ML library is linked, and no
accelerator runtime — those live behind the detector command, wherever it runs.

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
