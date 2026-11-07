// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mars04 {
inline constexpr std::uint16_t vendor_id = 0x040e;
inline constexpr std::uint16_t product_id = 0x4d35;
inline constexpr unsigned depth_width = 224, depth_height = 172;
inline constexpr unsigned rgb_width = 1344, rgb_height = 760;
inline constexpr std::size_t depth_packet_bytes = 1021952;
inline constexpr std::size_t rgb_packet_bytes = 1533952;
inline constexpr double depth_units_per_metre = 5460.0;

struct DeviceInfo {
    std::string firmware;
    std::string serial;
    int usb_speed_mbps = 0; // Negotiated link rate; zero when unknown/offline.
};
// fx, fy, cx, cy, k1, k2, p1, p2, k3 (Brown-Conrady model).
using Lens = std::array<float, 9>;
struct Point { float x, y, z; };
struct FrameMetadata {
    std::uint32_t sequence = 0;
    std::uint32_t device_timestamp = 0; // Raw counter; unit/epoch not established.
};
struct DepthFrame {
    FrameMetadata metadata;
    std::vector<std::int16_t> raw_depth;
};
struct RgbFrame {
    FrameMetadata metadata;
    std::vector<std::uint8_t> i420;
};
struct ImuSample {
    std::uint32_t device_timestamp = 0;
    // Legacy firmware's nine float channels, conventionally accel/gyro/mag XYZ.
    // Units, axes and sensor presence are NOT established. All-zero samples
    // have been observed on hardware; they are not valid motion measurements.
    std::array<float,9> channels{};
    bool has_nonzero_channels() const;
};
std::vector<ImuSample> parse_imu(const std::vector<std::uint8_t>&);

// Pure parsing/conversion functions, also useful for offline processing.
DeviceInfo parse_device_info(const std::vector<std::uint8_t>&);
Lens parse_lens(const std::vector<std::uint8_t>&);
DepthFrame parse_depth(const std::vector<std::uint8_t>&);
RgbFrame parse_rgb(const std::vector<std::uint8_t>&);
std::vector<Point> make_rays(unsigned width, unsigned height, const Lens&);
// Organized XYZ, same pixel order as depth. Invalid/non-positive depth -> (0,0,0).
std::vector<Point> point_cloud(const DepthFrame&, const std::vector<Point>& rays);
std::vector<std::uint8_t> rgb8(const RgbFrame&);
std::vector<std::uint16_t> depth_millimetres(const DepthFrame&);

class Camera {
public:
    // Claims interface 0 exclusively and resets this camera's USB queues.
    // Only this VID/PID and the validated type-2/V2.44 and type-4/V2.43
    // device/firmware pairs are supported (see README).
    explicit Camera(unsigned timeout_ms = 3000);
    ~Camera();
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;
    Camera(Camera&&) noexcept;
    Camera& operator=(Camera&&) noexcept;
    const DeviceInfo& info() const;
    const Lens& lens() const;
    // One reader per endpoint. read_depth and read_rgb may run concurrently.
    // Keep Camera alive and do not move/destroy it while readers are active.
    DepthFrame read_depth();
    RgbFrame read_rgb();
    // Experimental legacy-format IMU query, serialized internally. May be empty.
    // Check sample.has_nonzero_channels(); do not assume sensor availability.
    std::vector<ImuSample> read_imu();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mars04
