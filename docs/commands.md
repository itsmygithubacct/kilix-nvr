# Commands

Every `kilix-nvr` command, the camera grid, the user service, the camera check and display launches. Moved from the README.

```sh
kilix-nvr add       <name>          # add policy for a camera in cameras.conf
kilix-nvr set       <name> <k=v>... # change any capability afterwards
kilix-nvr cameras                   # what exists and what each one is doing
kilix-nvr check     <name>          # verify fresh video without recording
kilix-nvr view      [name]          # one camera, or all configured cameras in a grid
kilix-nvr run       [name...]       # run camera policies until stopped
kilix-nvr service   install         # install the user recorder service and prune timer
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

To show all configured cameras together, use `kilix-nvr view --no-detect`.
Four cameras form a 2×2 grid; they are ordered by their configured names.
The grid displays up to nine available feeds from at most 64 configured
policies. Larger policy lists require a named view, which looks up the camera
directly without the grid's list limit.
Press `g` to toggle the grid, Tab to cycle cameras, and `q` to quit.
`--no-detect` disables viewer object detection; it does not change an existing
recorder's policy or disable the viewer's motion and sound processing.

**Viewing does not record.** New camera policies have recording disabled.
Use `set` to select recording and retention, then `run` or the user service to
execute those policies. The event list and replay read this NVR's own store;
pointing a camera at another recorder's RTSP relay does not import that
recorder's history.

`kilix-nvr service install` creates missing user-unit directories, writes the
recorder and hourly retention units, then enables retention before restarting
the recorder. A retention setup failure returns an error and leaves an already
running recorder alone. Check `systemctl --user status kilix-nvr.service` and
`systemctl --user list-timers kilix-nvr-prune.timer` after installation.

Service installation snapshots the resolved NVR and RTSP data roots, plus any
explicit `KILIX_OBJECT_DETECTOR`, `KILIX_SOUND_CLASSIFIER`, and
`KILIX_RTSP_FFMPEG` overrides, in `<NVR root>/state/service.env` with mode
`0600`. The recorder and retention job use the same file. Re-run installation
after changing these settings; a shell export alone does not update a running
service. Explicit saved settings take precedence over the optional Kilix
environment file. Removing the service preserves this file and recordings.

Before opening a view, `kilix-nvr check doorway --seconds 10` tests the configured
substream (or main stream when no substream exists). It succeeds only after at
least three decoded frames and a frame less than one second old. It opens a
short independent session, stops it on success, timeout or interruption, and
does not start recording, detection or an event database. The timeout accepts
1–60 seconds; FFmpeg startup probing and process cleanup can add a few seconds.
Diagnostics do not print the camera URL. A failure can mean a missing LAN route
or tunnel, an unavailable camera, rejected credentials, or a local FFmpeg issue;
it does not establish which of those failed.

On Plebian OS, `pleb displays launch DP-1 -- kilix nvr view doorway` opens the
viewer in its own Kilix window on the named display, when that display command
is available. The viewer follows terminal resizing up to 3840×2160 pixels,
including a window maximized after startup. Resizing replaces the presentation
canvas without restarting its camera decoders. Use `pleb displays list` for connector names. A camera on another
subnet still needs a working route or a private relay/tunnel before the viewer
can receive frames. Keep its endpoint in the private `kilix-rtsp` configuration,
not in a desktop launcher or this checkout.
