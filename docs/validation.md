# Validation record

Tested on 2026-11-06. Hardware: Sunny Mars04 `040e:4d35`, firmware
`M04_V2.44.00.180322`, Phicomm N1, Debian 12 / Linux arm64, USB 2.0.
Compiler: GCC 12.2 in an arm64 Debian Docker container. Runtime capture was
performed on the N1 host as an ordinary user with the udev permission rule.

## Automated / build checks

- Release build and CTest: passed.
- Debug build with AddressSanitizer and UndefinedBehaviorSanitizer: passed.
- Installed CMake package: an external consumer using
  `find_package(mars04 CONFIG REQUIRED)` compiled, linked and ran successfully.
- Protocol tests use synthetic data; they require no attached camera.

## Hardware acquisition

`mars04-capture --check 120 --imu` captured:

| Stream | Count | Elapsed | Repeated sequences | Sequence discontinuities |
| --- | --- | --- | --- | --- |
| Depth | 120 | 7.99 s | 0 | 0 |
| RGB | 120 | 11.99 s | 0 | 0 |

The concurrent IMU probe produced 1198 syntactically valid records. Most
channels were zero; 58 records contained a repeated implausible value near
`-1e31` in one channel. Other diagnostic runs also encountered malformed record
markers. Moving/tilting the camera did not establish a credible gravity or gyro
response. **IMU measurements remain unvalidated.**

A separate three-pair capture produced RGB PNG, raw I420, depth PNG, PLY and
CSV metadata successfully. The first frame contained 28069 positive depth
pixels and the PLY contained exactly that many vertices.

Checks on the saved data:

- 16-bit depth PNG radial millimetres vs the corresponding PLY vector norms:
  maximum discrepancy 0.499 mm, consistent with integer millimetre rounding.
  This checks file encoding and geometric consistency, **not absolute accuracy**.
- RGB PNG vs FFmpeg's I420 decoding of the same raw frame:
  mean absolute channel difference 1.21/255, consistent with integer colour
  conversion rounding/implementation differences.
- Images and hardware-specific captures are kept outside this repository.

## Not established

Absolute range accuracy, photometric accuracy, IMU sensor availability/units,
cross-stream time synchronization, other firmware, multiple cameras, macOS,
Windows and USB 3.0 operation have not been validated.

## Graphical viewer

The SDL2/Cairo GUI was built in the same arm64 Docker environment and run on
the N1's 1920×1080 X11/XFCE desktop as the normal desktop user.

A 20-second timed run received 150 RGB frames, 276 depth frames and 1924 IMU
records, rendered 284 UI frames, and reported no acquisition error. One process
sample showed approximately 110 MiB RSS and 90% of one CPU core; this is an
observation, not a resource guarantee. No additional graphical runtime packages
were installed on that N1.

Actual mouse/keyboard interaction checks confirmed:

- Frozen RGB pixel regions stayed unchanged across two screenshots.
- Dragging the cloud while frozen changed the 3D projection.
- Resuming advanced the displayed frame sequence.
- Mouse-wheel zoom and reset controls were exercised.
- Saving produced a local screenshot and ASCII PLY cloud.
- Disconnect/reconnect reopened the camera and resumed incoming frames.

Screenshots were reviewed locally for image content, cloud rendering and text
layout; they contain private scene content and are not published.
