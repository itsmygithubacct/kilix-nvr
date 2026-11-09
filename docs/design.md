# Design

Why the recorder is shaped the way it is: one decode per camera, models outside the core, motion-gated crops, Matroska segments, retention and the filesystem as the record. Moved from the README.

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
worth reporting. It is fetched by hash rather than vendored. The sound-class
validation limitations above also apply to camera audio.

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

**Detection runs on crops around what moved**, not on whole frames — Frigate's
shape, in [`kilix-object-detect`](https://github.com/itsmygithubacct/kilix-object-detect).
A detector run on whole frames pays full price for every frame and sees a
person forty pixels tall as forty pixels; run on crops, it pays for the crops
and sees that person filling the frame it is given. It also means a preclusive
zone costs no inference at all, because a crop is never taken from one.

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
