# Tracking and zones

How detections become tracked objects, and how painted zone maps decide where they were. Moved from the README.

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
kilix-nvr zone add   sample-camera driveway inertia=3
kilix-nvr zone add   sample-camera road preclusive=yes
kilix-nvr zone paint sample-camera          # grabs a frame, hands over to kilix-mask
kilix-nvr zones      sample-camera
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
