# The sound-event model

Which model kilix-nvr listens with, why it and not the others, and what it
actually does on this fleet's cameras.

Written 2026-08-10, against the model as pinned in
[`tools/kilix-nvr-fetch-model`](../tools/kilix-nvr-fetch-model).

---

## What the choice had to satisfy

Three constraints, and together they decide it almost on their own.

1. **The licence has to be permissive.** This repository is published, so a
   model whose weights are research-only, or whose licence obliges anyone
   redistributing them to do something particular, is not usable here — however
   good it is.
2. **The input has to be what the pipe already carries.** `knvr_sound` pulls
   audio through ffmpeg as **16 kHz mono s16le** and hands the classifier one
   second at a time. A 32 kHz model means resampling every window and a bigger
   spectrogram for content the cameras do not transmit anyway.
3. **It has to be cheap enough to be beside a decode, not instead of one.**
   The whole reason motion gates detection is that CPU is the scarce thing.
   A sound model runs *continuously* — there is no motion gate for audio — so
   its per-second cost is paid by every camera with `sound_events=on`, all day.

Plus a fourth, discovered rather than assumed: **the cameras carry 8 kHz
audio**. All five on this fleet do — `pcm_alaw` on three, `aac` on two, every
one of them 8 kHz mono. So half of any model's trained bandwidth is empty
before it starts.

---

## What was considered

| | licence | params | input | AudioSet mAP | runtime |
| --- | --- | --- | --- | --- | --- |
| **YAMNet** | **Apache-2.0** | 3.7 M | **16 kHz** | 0.306 | TFLite, 4 MB |
| PANNs CNN14 | code Apache-2.0, **weights CC BY 4.0** | ~81 M | 32 kHz | 0.431 | PyTorch |
| EfficientAT `mn10_as` | MIT | ~4.9 M | 32 kHz | ~0.47 | PyTorch |
| AST | BSD-3-Clause | ~87 M | 16 kHz | ~0.46 | PyTorch |
| BEATs | MIT | ~90 M | 16 kHz | ~0.48 | PyTorch |

The mAP column is each model's published figure on AudioSet's own evaluation set — 521 classes of YouTube audio at full
bandwidth. It ranks these models against each other on *that*, which is not the
same question as "which one hears a dog through an 8 kHz camera microphone",
and nothing in the table should be read as if it were.

**Every one of these is permissively licensed.** That was the surprise: the
licensing question, which looked like the hard one, rules nothing out. PANNs is
the only one with a condition worth noticing — its weights are CC BY 4.0, so
attribution travels with them, which is fine but is a string attached to a
binary rather than to source.

What rules the others out is the runtime. **YAMNet is the only one of the five
that ships as a 4 MB TFLite file.** The rest are PyTorch checkpoints; running
any of them means torch, which is a gigabyte-scale dependency on every host
that watches a camera, to gain accuracy measured on audio the cameras cannot
send.

## The choice: YAMNet

MobileNet v1 over log-mel patches, 521 AudioSet classes, from Google's
TensorFlow Model Garden.

- **Licence: Apache-2.0.** The Kaggle model `google/yamnet` reports
  `licenseName: "Apache 2.0"` for its TFLite instance. The class *names* come
  from the AudioSet ontology (Gemmeke et al., ICASSP 2017), which is CC BY 4.0
  — so the attribution is recorded in the provenance file the fetcher writes.
- **Input: 15600 samples of 16 kHz mono**, 0.975 s. `knvr_sound` sends a
  16000-sample window and the listener passes the newest 15600 of it. No
  resampling, no buffering, no second rate to keep in step.
- **Cost: 3.5 ms per window, single-threaded** on a laptop-class x86 core with
  no accelerator — measured, median of twelve. That is 0.35% of one core per camera, which is what makes "no motion
  gate for audio" affordable.
- **Size: 4,126,810 bytes**, pinned by sha256.

It is the least accurate model in the table and it is still the right one:
accuracy the cameras cannot feed is not accuracy.

### Not vendored

The weights are not in this repository and will not be. `kilix-nvr-fetch-model`
pins the URL and the sha256, checks the hash **before** installing, and writes
a provenance file beside the model saying where it came from and under what
licence. A model that is not the model this was written against is worse than
no model: it answers confidently in an ontology nobody checked.

---

## From 521 classes to nine

AudioSet will name a hundred things that are not a reason to look at a
recording. The listener maps its output onto the nine kilix sound classes and
takes the **loudest member** of each group — "Bark" and "Bow-wow" are the same
dog, and summing them would make a confident dog out of two uncertain ones.

| kilix class | AudioSet names |
| --- | --- |
| speech | Speech, Child speech, Conversation, Narration |
| shout | Shout, Bellow, Whoop, Yell, Children shouting, Screaming |
| dog | Dog, Bark, Yip, Howl, Bow-wow, Growling, Whimper (dog) |
| glass | Glass, Chink/clink, Shatter, Breaking |
| alarm | Alarm, Alarm clock, Buzzer, Smoke detector, Fire alarm, Car alarm |
| siren | Siren, Civil defense siren, Police car, Ambulance, Fire engine, Emergency vehicle |
| gunshot | Gunshot/gunfire, Machine gun, Fusillade, Artillery fire, Cap gun |
| vehicle | Vehicle, Car, Truck, Bus, Motorcycle, Car passing by, Motor vehicle (road) |
| knock | Knock, Door, Doorbell, Ding-dong, Slam |

**The mapping is by name, not by index.** YAMNet's label list travels inside
the `.tflite` file as an appended zip, so the listener resolves names against
the model it actually loaded. Swap the model and the mapping re-resolves;
a name the new model does not have is reported at startup rather than
silently dropped. Indices would mean a different model reporting "gunshot"
for whatever now sits at 421.

`kilix-nvr-listen --list-classes` prints the resolved mapping.

---

## What it does, measured

Auditioned with `kilix-nvr-listen --wav`, and end to end through the C path
with `test-sound --audio FILE -- <listener>`, which is the same ffmpeg, the
same pipe and the same windows as a camera.

| input | reported |
| --- | --- |
| 9.8 s of narration (TTS) | **speech 1.00** |
| the same, band-limited to 8 kHz and back | **speech 0.99** |
| rifle shot, CC0 recording | gunshot 0.59 |
| the same at 8 kHz | gunshot 0.67 |
| shotgun pump, CC0 recording | gunshot 0.92 |
| 2m44s of music | nothing over 0.20 |
| 150 s of live audio, five cameras | nothing over 0.20 except one `shout 0.26` |
| 5 minutes each on four cameras | two quiet, `speech 0.33` on one, `speech 0.59` on one |

Two things worth taking from that.

**The 8 kHz band limit costs almost nothing** for the classes that matter here.
Speech went 1.00 → 0.99 and the rifle shot went *up*, which is within noise but
is certainly not the collapse the missing bandwidth suggested. This was the
main open risk and it did not materialise.

**Real scenes are mostly silent at the default threshold.** Two and a half
minutes across all five cameras produced not one class over 0.20, let alone the
0.50 default; the loudest thing anywhere was a `shout 0.26` that would not have
been reported.

**In twenty minutes, one window crossed it.** A five-minute sample from each of
four cameras produced exactly one score over 0.50: `speech 0.59`, one window,
with every other class at 0.03 or below — a clean, unambiguous answer rather
than a smear across confusable classes, which is what a false positive
usually looks like. Whether somebody actually spoke near that camera at that
moment is not something this can establish; the honest statement is that the
model was confident and nobody has listened to the second in question.

That is the shape of the number to expect: **roughly one candidate event per
twenty camera-minutes at the default threshold**, in a quiet suburban setting,
before any per-class tuning.

### What is not measured

- **Six of the nine classes have no positive control**: glass, dog, alarm,
  siren, vehicle and knock. The CC0 library on this machine is game foley, not
  security audio; its "breaking glass" clips are 0.7 s of tinkling that YAMNet
  reads as a coin dropping, which says more about the clips than about the
  model. Those six are untested and should be treated as untested.
- **No recall figure on camera audio.** Knowing it fires on a clean TTS voice
  is not knowing it will catch a shout in a garden at night.
- **No per-class thresholds.** One `min_score` governs all nine, and the right
  number for "speech" is very unlikely to be the right one for "glass".

---

## Running it

The core links no ML runtime — the listener is a subprocess, which is the whole
point. It needs a Python with a TFLite runtime in it:

```sh
python3 -m venv ~/.local/gpu_terminal/kilix-nvr/venv
~/.local/gpu_terminal/kilix-nvr/venv/bin/pip install ai-edge-litert numpy
kilix-nvr-fetch-model            # verifies the sha256 and writes provenance
```

Then tell kilix-nvr which Python to use. `KILIX_NVR_LISTEN` is the one place
that says where inference runs, and it is deliberately an environment variable
rather than a per-camera setting: every camera on a host reaches the models the
same way.

```sh
export KILIX_NVR_LISTEN="$HOME/.local/gpu_terminal/kilix-nvr/venv/bin/python \
                         /usr/local/bin/kilix-nvr-listen"
kilix-nvr set drivecam sound_events=on
```

`onnxruntime` works too — pass a `.onnx` as `--model` and the listener takes
that path instead. The label list then has to come from a sibling
`<model>.labels.txt`, since an ONNX file carries no metadata zip.

## If it is not good enough

The upgrade is **EfficientAT `mn10_as`**: MIT, 4.9 M parameters, and 0.471 mAP
against YAMNet's 0.306 (both published figures, neither measured here). The costs are torch and a 32 kHz resample per window,
and — on 8 kHz cameras — an unknown fraction of that improvement is
unreachable. Measure a real recall figure on camera audio before paying for it.
