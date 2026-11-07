# macOS viewer

The same C++17 driver and SDL2/Cairo viewer build as a native macOS `.app`.
Tested on Apple Silicon / macOS 27 with a real Mars04 connected at SuperSpeed
(5 Gbit/s). This is a locally ad-hoc-signed build, not a notarized distribution.

## Standard build

```sh
brew install cmake pkgconf libusb sdl2 cairo libpng
cmake -S . -B build-macos -DCMAKE_BUILD_TYPE=Release -DMARS04_BUILD_GUI=ON
cmake --build build-macos -j6
ctest --test-dir build-macos --output-on-failure
macos/package-app.sh
open "$HOME/Applications/Mars04 Camera Lab.app"
```

The package helper refuses to overwrite an existing app; pass a different
destination as its second argument when testing a new build. It embeds the
resolved non-system dylibs, rewrites their install names and ad-hoc signs the
result. `MARS04_LIBRARY_DIRS` can supply additional semicolon-separated library
search directories. Third-party libraries retain their respective licenses.
No driver/kernel extension or root permission was required on the tested Mac.

If macOS asks whether to allow the connected accessory, approve it yourself.
If the camera does not enumerate, unplug/replug it and check the cable and
connection. The camera is vendor-specific USB, not an ordinary UVC webcam.

## Tested development-machine workaround

The test Mac had a newer Command Line Tools SDK than the installed Xcode linker
could parse. Homebrew also lacked bottles for this prerelease OS. Rather than
changing the system developer-tool selection, the build explicitly selected
the SDK bundled inside Xcode using `-DCMAKE_OSX_SYSROOT=<compatible SDK path>`.
libusb 1.0.30, pkgconf 3.0.10 and SDL2 2.32.10 were built from their official
release sources into an ignored, project-local dependency prefix.

When building dependencies with Autotools, pass the compatible SDK to both
configure **and** make (or use explicit `-isysroot` compile/link flags). For CMake
pass `CMAKE_OSX_SYSROOT`. Provide the local `pkgconf` executable and dependency
prefix with `PKG_CONFIG_EXECUTABLE`, `CMAKE_PREFIX_PATH` and `PKG_CONFIG_PATH`.
The selected macOS SDK supplied zlib, bzip2 and Expat; local pkg-config metadata
for those system libraries was generated from the SDK headers. These local
workarounds are not required on a normal supported Homebrew/Xcode combination.
The macOS CMake path now locates Cairo's headers/dylib directly, avoiding the
SDK pkg-config metadata issue when dynamically linking the GUI.

## Controls and output

The viewer auto-connects. Drag the cloud to rotate, right-drag to pan, scroll to
zoom; use R to reset, D for density, Space for display freeze/resume and S to
save the view and cloud. Disconnect releases the USB interface. Window close,
Escape or the application's Quit command exits. Saved data goes to
`~/Mars04-captures/gui-<timestamp>/`.

The window fits the usable display area. Its USB indicator reflects the actual
negotiated link instead of always saying USB 2.0. Camera frames and captures
are local and are not uploaded by the application.

## Hardware validation

- Core protocol/geometry/colour/IMU malformed-input tests passed on Apple Clang.
- Native CLI: RGB/depth 60 frames each in approximately 4 seconds, no duplicate
  sequences or sequence discontinuities.
- Development GUI: 224 frames each in 15 seconds, 1490 IMU records, no query or
  capture errors; approximately 14.9 fps in both streams.
- Installed, bundled app: 449 RGB/depth frames each in 30 seconds, 2987 IMU
  records, no query/capture errors. Its actual macOS window was visually checked
  for live images, depth, point cloud and readable layout.
- The packaged app was verified with `codesign --verify --deep --strict`.

USB 3 operation alone did **not** raise the tested default stream rate above
15 fps. Higher acquisition modes still require further firmware/protocol
verification. The IMU's raw channel values remain unvalidated; zero/nonzero is
not a motion-validity test. Private camera images and device serials are not
included in the repository.

### High-rate packet delivery is not valid depth

A subsequent isolated C++ experiment requested 30 fps through the SDK-matched
runtime command. On USB 3 both RGB and depth **packets** arrived at about 30 fps,
but the processed depth plane became all zero. A fixed-scene validity check
after a complete power cycle measured:

| Stage | Depth packets | Frames with positive depth | Last valid pixels |
| --- | --- | --- | --- |
| Fresh default 15 fps | 61 | 61 | 25495 |
| Requested 30 fps | 121 | 0 | 0 |
| Restored 15 fps register | 61 | 0 | 0 |

SDK-matched mode/exposure commands did not recover valid processed depth in
another experiment. A complete camera power cycle restored default capture;
the restored CLI test produced 34505 positive depth pixels, and the installed
GUI again displayed a live cloud with about 34500 valid points.

Consequently the experimental setter and automatic 30 fps selection were
**not retained** in the public driver or installed app. The shipped app keeps
the working default mode. USB bandwidth is not the only issue: a correct
firmware-specific depth mode transition/calculation path is still missing.
RGB/packet FPS alone must not be reported as a usable 30 fps point-cloud mode.
