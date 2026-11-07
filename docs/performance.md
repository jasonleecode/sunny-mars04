# N1 frame-rate investigation

## Follow-up: direct connection restores dual 15 fps (2026-11-07)

After replacing the cable, the hub-connected camera stopped answering vendor
control requests; reconnect attempts timed out, and a USB reset failed to
enumerate it. The user unplugged/replugged the camera and connected the new
cable **directly to the N1, bypassing the hub**. Metadata queries and capture
then recovered. The unchanged capture tool read RGB and depth, 30 frames each,
in 2.034 s and 2.021 s respectively, without duplicate/discontinuous sequences.
The live viewer subsequently displayed approximately **14.9 fps on both streams**.
The direct connection still negotiated USB 2.0 high speed (480 Mbit/s).

This demonstrates that the earlier dual-stream slowdown is not an unavoidable
N1 compute limit or fixed firmware FPS ceiling. The original hub connection
path is implicated. Power cycling and connection topology changed together,
so these observations do not distinguish hub power/signal issues from its
scheduling or the camera's previous stalled state. The new cable is capable
of working with the camera in the direct setup. No driver change was needed
to restore capture; the viewer's Reconnect button reopened it.

The original measurements below describe the **hub-connected setup**.

Measured on N1 / legacy Mars04 firmware, 2026-11-06. Observations on one setup,
not a universal camera specification. USB connection: high speed, 480 Mbit/s,
through a USB 2.0 hub.

## Transport baseline without GUI or image processing

| Readers enabled | RGB | Depth |
| --- | --- | --- |
| RGB only | 14.93 fps | — |
| Depth only | — | 15.06 fps |
| Both | 7.55 fps | 14.96 fps |
| Both + IMU | 7.52 fps | 14.82 fps |

Repeating after reconnecting gave RGB-only 14.97 fps, dual RGB 7.7–8.2 fps and
depth about 15 fps. Short initial GUI averages were sometimes higher; use the
current one-second rate to avoid mistaking a startup burst for steady throughput.
This establishes that slowdown can occur before conversion or point-cloud
generation. IMU was not the dominant cost. It does not isolate firmware
scheduling from host-controller/hub scheduling.

## Payload and scheduling

RGB transfers are 1,533,952 bytes each; depth transfers are 1,021,952 bytes,
although processed depth alone occupies 77,056 bytes. The remaining depth
packet includes gray data, raw phase data, auxiliary data and padding. Complete
packets must be consumed to preserve boundaries.

Both streams at 15 fps would need **38.3 MB/s** of payload, excluding USB
overhead. Advertised 480 Mbit/s is not a payload-throughput guarantee. Large
depth transfers and uneven camera/USB scheduling are plausible bottlenecks.

Pauses between depth reads sometimes raised RGB to 11–13 fps in a transport-only
test, with depth about 14–15 fps. This benefit was not reproducible in the full
GUI; longer pauses reduced depth rate. No experimental pause is retained.
16/64/256 KiB read chunks did not improve RGB and often reduced depth rate.

A matching SDK command requesting 30 fps did not improve measured dual-stream
rates. A subsequent USB reset failed to enumerate the camera; unplugging and
replugging recovered it. No frame-rate setter or firmware update is exposed by
this driver.

## CPU and display

A pre-optimization viewer sample measured RGB conversion about 10.7 ms, depth
geometry/preview about 6.6 ms, UI drawing about 28.8 ms and UI presentation about
14.2 ms. Depth colours are now precomputed and reused by the organized point
cloud. Subsequent samples measured geometry/preview about **2.4–2.7 ms**, drawing
about **23 ms**, presentation about **13 ms**. These separate runs are indicative,
not controlled scene benchmarks. Lower CPU cost did not establish a sustained
camera FPS increase. An extra RGB conversion thread also failed to improve
sustained rate and was not retained.

The GUI intentionally refreshes at about **15 Hz**, independently of acquisition.
The tested depth source also delivers about 15 fps. A faster redraw alone would
repeat depth frames. LocalSend used almost one CPU core during part of this
investigation; it was not stopped by the test. CPU frequency was at its
configured 1.512 GHz maximum. Thermal throttling was not established as the
main cause.

## Timed report

```sh
mars04-viewer --run-seconds 20 --report /tmp/mars04-timing.json
```

`timing_ms` contains mean wall-clock durations, not per-thread CPU times:
`rgb_read`, `rgb_convert`, `depth_read`, `depth_geometry_preview`, `ui_draw`,
`ui_present`. Reads include USB waiting and parsing. The last approximately
one-second RGB/depth rates are also recorded.

An optimized hub-connected viewer run still spent about 123 ms reading RGB and
10 ms converting it, consistent with roughly 7.5 fps delivery in that run.
The direct-connection follow-up above demonstrates dual 15 fps is possible.
FPS counters and faster redraw timers do not create additional camera frames.
