# kilix-nvr

A network video recorder that lives in a terminal.

`kilix-nvr` records RTSP cameras, detects people and animals in what they see,
and lets you review what happened — without a browser anywhere in it. Live
views, playback and the review interface all render through the [Kitty graphics
protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/), using
[`kilix-rtsp`](https://github.com/itsmygithubacct/kilix-rtsp) for acquisition
and presentation.

**Status: it works.** Cameras record, motion gates a detector, detections
become tracked objects, zones say where they were, sound events are heard,
events are stored and reviewable, and retention deletes. Built and verified
against live cameras. What is not here yet: per-class sound thresholds, and a
positive control for six of the nine sound classes — see
[`docs/SOUND-MODEL.md`](docs/SOUND-MODEL.md) for exactly what has and has not
been measured.

```sh
git submodule update --init --recursive
make && make test
```

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
| zone map | a kilix-mask file | — |
| decode size | W×H | camera's substream |
| container | `mkv` / `mp4` / `mov` | `mkv` |
| retain days | days this camera keeps | — |

**A newly added camera is viewable and records nothing.** Nothing starts
consuming disk without being asked for.

Object detection implies motion detection, which is its gate. Motion detection
on its own is a complete configuration — events for movement, without ever
asking a model what caused it.

## Tracking and zones

**Detections become objects.** A detector answers "what is in this frame" and
nothing else; ask it twice and you get two unrelated answers. Tracking is the
missing noun — the same car, seen repeatedly, with a beginning and an end — and
it is what turns "four hundred detections" into "three people and a car". Box
overlap plus a centroid gate, matched within a class, no Kalman filter and no
scipy: `kilix-nvr objects <event>` reads the result.

A track that stops moving for thirty seconds is marked **parked**, because a
car on the drive since Tuesday must stop being news.

**Zones are painted, not typed.** A camera's zone map is a
[kilix-mask](https://github.com/itsmygithubacct/kilix-mask) file: one named
region per zone, painted over a frame from that camera. The painter, the file
format, the names and the free-form attributes all already existed, and the map
opens in any image viewer. The cost is that regions cannot overlap — a point is
in exactly one zone or none.

```sh
kilix-nvr zone add   drivecam driveway inertia=3
kilix-nvr zone add   drivecam road preclusive=yes
kilix-nvr zone paint drivecam          # grabs a frame, hands over to kilix-mask
kilix-nvr zones      drivecam
kilix-nvr events --zone driveway
```

Policy rides in each region's attributes, so a zone map is self-describing:

- `inertia=N` — frames inside before the object counts as having arrived.
- `preclusive=yes` — activity here **suppresses** rather than raises, which is
  ZoneMinder's term and its semantics. It applies at the motion gate, so a
  preclusive zone costs no detector time at all. Suppressed regions are
  counted and reported, because a suppression nobody can see is
  indistinguishable from a camera that stopped working.
- `loiter=SECONDS` — how long dwelling there becomes interesting by itself.

The point tested is the middle of the box's bottom edge — where the thing
touches the ground. A centroid puts a tall person in the zone their chest is
over, which for a camera looking down a drive is routinely the wrong one.

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

**It hears as well as sees.** Sound events are a second subprocess on the same
480-byte contract, with its own ffmpeg pulling `-vn` audio: different models,
different rates and different failure modes, and a wedged audio model must not
stop a camera seeing. The model is YAMNet — Apache-2.0, 4 MB, 16 kHz mono,
3.5 ms per second of audio — mapped from its 521 AudioSet classes onto nine
worth reporting. It is fetched by hash rather than vendored:
[`docs/SOUND-MODEL.md`](docs/SOUND-MODEL.md) is why that model and what it
actually does on 8 kHz camera audio.

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

**Segments are Matroska.** mp4 cannot mux the `pcm_alaw` audio these cameras
commonly carry — `-c copy` refuses outright — and MPEG-TS accepts it while
silently dropping the audio stream. mkv keeps both streams byte-exact and
tolerates a truncated segment, which mp4 does not. Overridable per camera; there
is no browser here to have an opinion.

**Retention is per-camera days with a global size cap behind it.** Days express
intent, the cap is the guarantee: a camera that simply gets busier produces more
bytes for the same number of days, so intent alone does not bound size.

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

## Commands

```sh
kilix-nvr add       <name> <url>    # onboard a camera; prompts for capabilities
kilix-nvr set       <name> <k=v>... # change any capability afterwards
kilix-nvr cameras                   # what exists and what each one is doing
kilix-nvr watch     <name>          # run one camera, honouring its config
kilix-nvr events    [--since ...]   # query what happened
kilix-nvr review                    # review interface
kilix-nvr play      <event|time>    # playback
kilix-nvr reanalyze <event>         # re-run detection over stored footage
kilix-nvr clip      <event>         # cut it out of the segments, -c copy
kilix-nvr objects   <event>         # the things it was, not the frames
kilix-nvr zones     <name>          # a camera's zones and what they do
kilix-nvr zone      add|remove|paint <name> ...
kilix-nvr prune                     # apply retention
```

There is no separate `record` or `detect` command: what a camera does is its
configuration, not an invocation.

`clip <event>` cuts an event out of the segments without re-encoding, and
`watch <name>` is what runs one camera in the foreground.

## Dependencies

A C11 compiler, POSIX, pthreads, and `sqlite3` at link time. At runtime: the
`ffmpeg` binary and a detector command. No ML library is linked, and no
accelerator runtime — those live behind the detector command, wherever it runs.

**Where inference runs is an environment variable**, since it is a property of
the host rather than of a camera:

```sh
KILIX_NVR_DETECT="ssh gpubox kilix-nvr-detect"
KILIX_NVR_LISTEN="$HOME/.local/gpu_terminal/kilix-nvr/venv/bin/python \
                  /usr/local/bin/kilix-nvr-listen"
```

Unset, each falls back to `kilix-nvr-detect` and `kilix-nvr-listen` on `PATH`,
which `make install` puts there beside the binary. Both are split on spaces
with no quoting: a path containing a space needs a wrapper script, which is a
smaller surprise than half-implemented shell quoting.

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
