# Sunny Mars04 USB driver

An independent C++17/libusb driver for the Sunny Optical Mars04 ToF camera
(`040e:4d35`, USB product string **Sunny Mars04**). MIT licensed.

中文：面向舜宇 Mars04 的 C++ / CMake 用户态驱动，支持 RGB、深度和标定 XYZ
点云。已在斐讯 N1 的 Debian 12 / arm64 环境实机验证。无需厂商 SDK、Python、
OpenCV 或 ROS。IMU 目前仅支持实验性协议诊断，尚未取得可信的运动测量数据。

## Supported hardware and outputs

| Feature | Status |
| --- | --- |
| Firmware | Only `M04_V2.44.00.180322`; other versions are rejected |
| RGB | 1344×760, I420 planar YUV, RGB PNG and raw `.yuv` |
| Depth | 224×172, 16-bit radial distance PNG in millimetres; zero invalid |
| Point cloud | Calibrated XYZ in metres, binary little-endian PLY |
| USB transport | USB 2.0 bulk transfer, concurrent RGB/depth readers |
| IMU | Experimental raw legacy reply parsing and CSV; **not validated motion data** |
| RGB/depth/IMU synchronization | Not implemented; raw device counters preserved |
| Colourized point cloud | Not implemented |

Depth conversion uses the device's lens calibration and the matching SDK's
range scale. Absolute distance accuracy has not been checked against a measured
target. The old firmware does not supply the newer SDK's depth CRC.

## Live graphical test program

`mars04-viewer` is a C++ desktop application for live RGB, depth, an interactive
XYZ point cloud and IMU diagnostics. It uses SDL2 and Cairo with a CPU renderer;
no OpenGL, Qt, browser or Python runtime is required. It was tested on the N1
X11/XFCE desktop. The interface uses English labels and the desktop launcher has
a Chinese name on Chinese desktops.

中文：运行后自动连接相机，同时显示彩色图像、彩色深度图、可旋转缩放的实时点云和
IMU 九通道原始值/变化曲线。IMU 明确标记为“运动数据未验证”，异常大值不绘入曲线。
桌面快捷方式名称为“**Mars04 相机测试**”。

```sh
sudo apt-get install libsdl2-dev libcairo2-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMARS04_BUILD_GUI=ON
cmake --build build -j2
sudo cmake --install build
mars04-viewer
```

The GUI is optional and disabled by default. It needs the same camera USB
permissions as the CLI. Runtime dependencies are `libsdl2-2.0-0`, `libcairo2`,
libusb, and a sans-serif font (for example `fonts-dejavu-core`).

| Action | Control |
| --- | --- |
| Rotate cloud | Left-drag inside the cloud viewport |
| Pan cloud | Right-drag inside the cloud viewport |
| Zoom cloud | Mouse wheel over the cloud panel |
| Reset cloud view | **Reset 3D** button or `R` |
| Full / half point density | `D` |
| Freeze / resume display | **Freeze / Resume** button or Space |
| Release / reopen the camera | **Disconnect / Reconnect** button |
| Save screenshot and visible cloud | **Save view** button or `S` |
| Exit | Window close button or Escape |

Freeze pauses the display while acquisition continues. Disconnect releases the
camera, preserving the last frames on screen. A failed connection or unplugged
camera shows an error; reconnect explicitly after correcting it. Saving creates
`~/Mars04-captures/gui-<timestamp>/view.png` and `cloud.ply` (ASCII XYZ in metres).
These are local captures and are not included in the repository.

The GUI bounds its buffers to the newest frame per image stream and 150 IMU
samples, rather than building a growing playback queue. The RGB preview is
downsampled to 672×380; the CLI still saves full-resolution RGB. Depth and point
cloud use the complete 224×172 depth data. Cloud colour represents radial range
on a 0–4 m scale; it is not projected RGB colour. The default 3D view is centered
on the current valid cloud centroid. Streams are independent, not synchronized.

IMU readings are explicitly experimental. The interface preserves raw values
and raw timestamps, shows parser-error counts, and omits magnitudes ≥10000 from
the autoscaled plots so firmware garbage does not destroy the plot scale.
This threshold is a display guard, **not a physical validity criterion**.

For a timed hardware/UI check:

```sh
mars04-viewer --run-seconds 20 --screenshot /tmp/view.png --report /tmp/view.json
```

The report records received RGB/depth frames, IMU records, valid cloud points,
rendered frames and acquisition errors. A timed run returns failure if no RGB
or depth frames were received. With no camera the UI still opens with an error
and a reconnect button. The `--help` option requires neither a camera nor X11.

Build the viewer in Docker with
`docker build --platform linux/arm64 --build-arg MARS04_BUILD_GUI=ON -t sunny-mars04 .`;
copy `/src/build/mars04-viewer` from the resulting container onto the N1.

Timed GUI reports also include per-stage wall-clock timings and the latest RGB/depth
rate. See the [N1 frame-rate investigation](docs/performance.md) for measured USB
behaviour, tested alternatives and the scope of the CPU optimization.

## Build on Debian / Ubuntu

```sh
sudo apt-get install build-essential cmake pkg-config libusb-1.0-0-dev libpng-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

For a library-only build use `-DMARS04_BUILD_TOOLS=OFF`. The library needs
libusb; libpng is used only by the capture tool. C++17, CMake ≥ 3.16 required.
Linux is the supported and tested platform.

### Docker build

```sh
docker build --platform linux/arm64 -t sunny-mars04 .
container=$(docker create sunny-mars04)
docker cp "$container:/src/build/mars04-capture" ./mars04-capture
docker rm "$container"
```

This builds and runs offline tests in an arm64 Debian container. Run the binary
on the N1 with `libusb-1.0-0` and `libpng16-16` installed. Accessing the camera
from Docker itself is not necessary; capture is performed on the N1 host.

### USB permissions

```sh
sudo install -m 644 udev/70-sunny-mars04.rules /etc/udev/rules.d/
sudo usermod -aG video "$USER"
sudo udevadm control --reload-rules
```

Reconnect the camera and log in again if the group membership changed.
Do not run another capture process against the same camera simultaneously.
Opening a camera claims interface 0 and resets **that camera's** USB queues.
The firmware starts outputting images by itself; this driver does not flash
firmware, change calibration or perform persistent configuration writes.

## Capture

```sh
mars04-capture --info
mars04-capture --output capture-001 --frames 3
mars04-capture --check 120
mars04-capture --output capture-with-imu --frames 30 --imu
mars04-capture --imu-only 10 --output imu-diagnostic
```

The output directory must be new. With no arguments the tool saves one frame
pair to a timestamped directory in the current working directory.

Each image pair produces `0000-rgb.png`, `0000.yuv`, `0000-depth-mm.png` and
`0000.ply`. `rgb.csv` and `depth.csv` retain independent device sequence numbers
and raw timestamps; the same file number does **not** imply synchronized capture.
`capture.json` records calibration and acquisition statistics. It contains your
device serial number: review it before sharing a capture.

PLY coordinates are X right, Y down, Z forward. PLY files omit non-positive depth.
The C++ API returns organized XYZ, with invalid points represented as `(0,0,0)`.
Depth PNG stores **radial range**, not Z distance. RGB conversion assumes
BT.601 limited range; no photometric calibration has been performed.

To inspect raw colour frames with FFmpeg:

```sh
ffplay -f rawvideo -pixel_format yuv420p -video_size 1344x760 capture-001/0000.yuv
```

### IMU limitations

The camera responds to the IMU query with timestamped records, but tested
hardware returns zero in the expected accelerometer/gyroscope channels,
including during the motion test. Occasional malformed records and nonzero
garbage were also observed. This is **not evidence of a working IMU sensor**.

`--imu` and `--imu-only` are diagnostic options. They preserve nine raw float
channels, the raw device timestamp and a host steady-clock timestamp in
`imu.csv`. Channel semantics, units, axis directions and hardware population
remain unverified. `has_nonzero_channels` is merely a numerical flag;
**nonzero does not mean valid**. The manifest always reports
`motion_data_validated: false`. Malformed query replies are counted and skipped.
No orientation, sensor fusion or magnetic-field measurements are claimed.

## Use from C++

After installation:

```cmake
find_package(mars04 CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE mars04::mars04)
```

```cpp
#include <mars04/mars04.hpp>

mars04::Camera camera;
const auto rays = mars04::make_rays(224, 172, camera.lens());
auto depth = camera.read_depth();
auto xyz = mars04::point_cloud(depth, rays);
auto rgb = mars04::rgb8(camera.read_rgb());
auto imu_diagnostics = camera.read_imu();
```

One thread may read depth while another reads RGB. IMU control queries are
serialized internally and may run alongside bulk readers. Do not move or
destroy `Camera` until readers have finished. Errors throw `std::exception`
subclasses. If a short transfer or boundary error occurs, close and reopen the
camera. Multiple matching devices and hotplug reconnection are not implemented.

## Protocol and provenance

See [the protocol notes](docs/protocol.md) for packet layouts, observed firmware
differences and validation scope. This is an independently written implementation
based on device observations and analysis of publicly available Sunny SDK
interfaces/binaries in [BluewhaleRobot/sunny_tof_node](https://github.com/BluewhaleRobot/sunny_tof_node).
No vendor SDK source, headers, libraries, disassembly, firmware, calibration
dumps or camera captures are distributed in this repository. The project is not
affiliated with Sunny Optical or MCCI.

Tests use synthetic packets and analytical geometry/colour fixtures. Run with
`-DMARS04_SANITIZERS=ON` in a Debug build to enable ASan/UBSan. No camera is
needed for the offline tests. Hardware validation is a separate step.
See the [hardware and build validation record](docs/validation.md) for measured
results and their limits.
