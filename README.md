# kilix-nvr

A network video recorder that lives in a terminal.

**Status: it works.** Cameras record, motion gates a detector, detections
become tracked objects, zones say where they were, sound events are heard,
events are stored and reviewable, and retention deletes. Built and verified
against live cameras. What is not here yet: per-class sound thresholds, and a
positive control for six of the nine sound classes. Sound-model coverage is
incomplete; do not treat the available classes as equally validated.

**Delivery:** Kilix runs it as `kilix nvr` (aliases `record`, `recorder`),
which clones the commit Kilix pins into
`~/.local/gpu_terminal/sources/kilix-apps/kilix-nvr` with its submodules and
builds it. The Kilix Content catalog lists the same commit as the app **Camera
Recorder** (`kilix install kilix-nvr`) and has a system app, **Camera Wall**,
that runs `kilix nvr view`. That pin, `e68b5d14`, is on the
`work/023-rc1-closure` branch, nine commits ahead of `main`; this README
describes `main`. No detector model ships with it: `kilix install yolox` and
`kilix yamnet install` build the detector runtimes and fetch the YOLOX and
YAMNet models through Kilix Content on first use. On Plebian-OS 0.2.3 those
models (`yolox_nano`, `yolox_s`, `yamnet`) are already installed at first boot
from the ISO's offline default model bundle.

## What it does

`kilix-nvr` records RTSP cameras, detects people and animals in what they see,
and lets you review what happened — without a browser anywhere in it. Live
views, playback and the review interface all render through the [Kitty graphics
protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/), using
[`kilix-rtsp`](https://github.com/itsmygithubacct/kilix-rtsp) for acquisition
and presentation.

- Records each camera as stills, clips or continuous `-c copy` segments, with
  per-camera retention behind a global size cap.
- Gates object detection behind motion, runs it on crops, tracks what it finds
  and places it in painted zones.
- Hears camera audio through a sound classifier on the same pipe contract.
- Shows a live view, a camera grid, an event list, review and playback, all in
  the terminal.

## Quick start

```sh
git submodule update --init --recursive
make && make test
make install PREFIX="$HOME/.local"   # kilix-nvr, kilix-look-detect and the sound tools
```

`make` alone leaves the command at `build/kilix-nvr`; the commands below assume
it is on `PATH`. Inside Kilix, `kilix nvr ARGS` runs an installed `kilix-nvr`,
or builds the commit Kilix pins the first time.

Personal camera names, URLs, credentials, recordings, and deployment notes must stay outside this repository. Keep camera configuration in `~/.local/gpu_terminal/kilix-rtsp/config/cameras.conf` with mode `0600` and private parent directories. Repository examples use synthetic camera names.

Define the camera in `cameras.conf` (see
[docs/configuration.md](docs/configuration.md)), then add policy for it, check
it and watch it:

```sh
kilix-nvr add doorway
kilix-nvr check doorway --seconds 10
kilix-nvr view doorway
kilix-nvr set doorway record=continuous motion=on
kilix-nvr service install
```

## Usage

| Command | What it does |
|---|---|
| `kilix-nvr add NAME` / `set NAME k=v...` | Add policy for a camera from `cameras.conf`, or change any capability. |
| `kilix-nvr cameras` / `check NAME` | What each camera is doing; verify fresh video without recording. |
| `kilix-nvr view [NAME]` | One camera, or every configured camera in a grid (`g`, Tab, `q`). |
| `kilix-nvr run [NAME...]` / `watch NAME` | Run camera policies until stopped, or one camera in the foreground. |
| `kilix-nvr service install` | Install the user recorder service and the hourly retention timer. |
| `kilix-nvr events` / `review` / `play` / `clip` / `objects` | Query, browse, play back and cut events, and list their tracked objects. |
| `kilix-nvr reanalyze EVENT` | Re-run detection over stored footage. |
| `kilix-nvr zones NAME` / `zone add\|remove\|paint NAME ...` | List and edit a camera's painted zones. |
| `kilix-nvr prune` | Apply retention. |
| `make && make test` | Build and run the C, shell and Python tests. |

Every option and the viewer details are in [docs/commands.md](docs/commands.md).

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

## How it works

```
camera --RTSP, one session--> ffmpeg decode (kilix-rtsp) --> shared-memory frame ring
   |                                                  |            |           |
   | -c copy packets                       motion gate (luma)   live view   review
   v                                       kilix-motion-detect
Matroska segments on disk                             | crops around what moved
                                                      v
                       detector subprocess: tensor in, float32[20][6] out
                       (default: kilix-look-detect -> kilix yolox run)
camera audio --ffmpeg -vn--> sound classifier, same 480-byte contract (YAMNet)
                                                      |
                 tracks -> zones (kilix-mask maps) -> events: files + SQLite index
```

Each camera has exactly one RTSP session and one decode; motion detection, the
object detector, the live view and the recorder all read the same frames from a
shared-memory ring, and continuous recording copies the camera's own packets
without decoding them. Inference never links into the recorder: object and
sound detection are supervised subprocesses on a fixed-size pipe, so the
command can run locally or on another machine. Detections become tracks, a
track's ground point is placed in a painted zone, and zone attributes decide
whether it raises an event or suppresses one. Events live on disk by camera and
timestamp, and SQLite is a rebuildable index over them. The reasons are in
[docs/design.md](docs/design.md) and [docs/zones-and-tracking.md](docs/zones-and-tracking.md).

## Documentation

- [docs/zones-and-tracking.md](docs/zones-and-tracking.md): tracks, parked
  objects and painted zone maps with their attributes.
- [docs/design.md](docs/design.md): shared decode, sound, the detector pipe,
  crops, motion gating, segments, retention and the filesystem index.
- [docs/commands.md](docs/commands.md): every command, the camera grid, the
  user service, the camera check and display launches.
- [docs/configuration.md](docs/configuration.md): dependencies, where
  inference runs, data locations and the camera secret file.

## Related repositories

- [kilix-rtsp](https://github.com/itsmygithubacct/kilix-rtsp#readme): camera
  acquisition and presentation, and the shared `cameras.conf`.
- [kilix-motion-detect](https://github.com/itsmygithubacct/kilix-motion-detect#readme),
  [kilix-object-detect](https://github.com/itsmygithubacct/kilix-object-detect#readme)
  and [kilix-sound-detect](https://github.com/itsmygithubacct/kilix-sound-detect#readme):
  the motion gate, crop detection and sound events, as submodules.
- [kilix-mask](https://github.com/itsmygithubacct/kilix-mask#readme): the
  zone painter and file format.
- [kilix-yolox](https://github.com/itsmygithubacct/kilix-yolox#readme): the
  default object detector behind `kilix yolox run`.
- [kilix-tui-utils](https://github.com/itsmygithubacct/kilix-tui-utils#readme):
  Camera Manager, for camera profiles and groups.
- [kilix-content](https://github.com/itsmygithubacct/kilix-content#readme): the
  catalog entries and the detector model assets.

## Part of Plebian-OS

kilix-nvr is one of the repositories behind
[Plebian-OS](https://github.com/itsmygithubacct/plebian-os#readme), a Debian
install whose desktop is [Kilix](https://github.com/itsmygithubacct/kilix#readme)
running the [Kilix 95](https://github.com/itsmygithubacct/kilix-95#readme)
desktop through [Pleb](https://github.com/itsmygithubacct/pleb#readme).
It is a content-catalog app: Kilix fetches and builds a pinned commit when a
user installs Camera Recorder or first runs `kilix nvr`. Its detector models
come from the ISO's offline default model bundle at first boot. The map of every repository and how it ships is on the
[components page](https://github.com/itsmygithubacct/plebian-os/blob/main/docs/components.md).

## License

MIT. See `LICENSE`.
