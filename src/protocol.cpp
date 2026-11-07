// SPDX-License-Identifier: MIT
#include "mars04/mars04.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mars04 {
namespace {
std::uint16_t u16(const std::vector<std::uint8_t>& b, std::size_t p) {
    return static_cast<std::uint16_t>(unsigned(b.at(p)) | (unsigned(b.at(p+1)) << 8));
}
std::uint32_t u32(const std::vector<std::uint8_t>& b, std::size_t p) {
    return std::uint32_t(u16(b,p)) | (std::uint32_t(u16(b,p+2)) << 16);
}
std::string text(const std::vector<std::uint8_t>& b, std::size_t start, std::size_t len) {
    std::string s;
    for (std::size_t i=start; i<start+len && b.at(i); ++i) {
        if (b[i]<32 || b[i]>126) throw std::runtime_error("Invalid device text field");
        s+=static_cast<char>(b[i]);
    }
    return s;
}
FrameMetadata metadata(const std::vector<std::uint8_t>& b, std::size_t offset) {
    return {u32(b,offset+32),u32(b,offset+36)};
}
std::uint8_t clip(int x) { return static_cast<std::uint8_t>(std::clamp(x,0,255)); }
}

DeviceInfo parse_device_info(const std::vector<std::uint8_t>& b) {
    if (b.size()<64) throw std::runtime_error("Truncated device info");
    const auto type=u32(b,0);
    if ((type!=2 && type!=4) || u16(b,8)!=depth_width || u16(b,10)!=depth_height+1 ||
        u16(b,14)!=rgb_width || u16(b,16)!=rgb_height)
        throw std::runtime_error("Unsupported device type or dimensions");
    DeviceInfo result{text(b,24,20),text(b,44,19)};
    if (!((type==2 && result.firmware=="M04_V2.44.00.180322") ||
          (type==4 && result.firmware=="M04_V2.43.01.180307")))
        throw std::runtime_error("Unsupported device/firmware combination: "+result.firmware);
    return result;
}

Lens parse_lens(const std::vector<std::uint8_t>& b) {
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559,
                  "IEEE 754 binary32 required");
    if (b.size()<36) throw std::runtime_error("Truncated lens calibration");
    Lens lens{};
    for (std::size_t i=0; i<lens.size(); ++i) {
        auto bits=u32(b,i*4);
        std::memcpy(&lens[i],&bits,4);
        if (!std::isfinite(lens[i])) throw std::runtime_error("Non-finite lens calibration");
    }
    if (lens[0]<=0 || lens[1]<=0) throw std::runtime_error("Invalid focal length");
    return lens;
}

bool ImuSample::has_nonzero_channels() const {
    return std::any_of(channels.begin(),channels.end(),[](float x){return x!=0;});
}
std::vector<ImuSample> parse_imu(const std::vector<std::uint8_t>& b) {
    // Legacy M04_V2.44 reply differs from newer SDK's half-float layout.
    // Header: count byte + 3 reserved; each record: "IMU\0", u32 time, 9 f32.
    if (b.size()!=512) throw std::runtime_error("IMU reply must contain 512 bytes");
    const std::size_t count=b[0];
    if (count>(b.size()-4)/44) throw std::runtime_error("Invalid IMU record count");
    std::vector<ImuSample> result;result.reserve(count);
    for (std::size_t i=0; i<count; ++i) {
        const auto offset=4+i*44;
        if (u32(b,offset)!=0x00554d49) throw std::runtime_error("Unrecognized IMU record format");
        ImuSample sample;sample.device_timestamp=u32(b,offset+4);
        for (std::size_t j=0; j<9; ++j) {
            auto bits=u32(b,offset+8+j*4);
            std::memcpy(&sample.channels[j],&bits,4);
            if (!std::isfinite(sample.channels[j])) throw std::runtime_error("Non-finite IMU channel");
        }
        result.push_back(sample);
    }
    return result;
}

DepthFrame parse_depth(const std::vector<std::uint8_t>& b) {
    constexpr std::size_t pixels=depth_width*depth_height, offset=pixels*5;
    if (b.size()!=depth_packet_bytes) throw std::runtime_error("Depth packet length mismatch");
    if (u32(b,offset+16)!=depth_width)
        throw std::runtime_error("Depth metadata mismatch; frame boundary lost");
    DepthFrame frame{metadata(b,offset),{}};
    frame.raw_depth.resize(pixels);
    for (std::size_t i=0; i<pixels; ++i) {
        const auto value=u16(b,i*2);
        frame.raw_depth[i]=static_cast<std::int16_t>(value<32768 ? int(value) : int(value)-65536);
    }
    return frame;
}

RgbFrame parse_rgb(const std::vector<std::uint8_t>& b) {
    constexpr std::size_t bytes=rgb_width*rgb_height*3/2;
    if (b.size()!=rgb_packet_bytes) throw std::runtime_error("RGB packet length mismatch");
    if (u32(b,bytes+16)!=rgb_width || u32(b,bytes+20)!=rgb_height)
        throw std::runtime_error("RGB metadata mismatch; frame boundary lost");
    return {metadata(b,bytes),std::vector<std::uint8_t>(b.begin(),b.begin()+bytes)};
}

std::vector<Point> make_rays(unsigned w, unsigned h, const Lens& lens) {
    if (!w || !h || w>4096 || h>4096) throw std::invalid_argument("Invalid ray image dimensions");
    for (auto f:lens) if (!std::isfinite(f)) throw std::invalid_argument("Invalid lens calibration");
    if (lens[0]<=0 || lens[1]<=0) throw std::invalid_argument("Invalid focal length");
    const auto [fx,fy,cx,cy,k1,k2,p1,p2,k3]=lens;
    std::vector<Point> rays;
    rays.reserve(std::size_t(w)*h);
    for (unsigned v=0; v<h; ++v) for (unsigned u=0; u<w; ++u) {
        const double xd=(double(u)-cx)/fx, yd=(double(v)-cy)/fy;
        double x=xd,y=yd;
        for (int i=0; i<100; ++i) {
            const double r2=x*x+y*y;
            const double radial=1+k1*r2+k2*r2*r2+k3*r2*r2*r2;
            const double dx=2*p1*x*y+p2*(r2+2*x*x);
            const double dy=p1*(r2+2*y*y)+2*p2*x*y;
            if (!std::isfinite(radial) || std::abs(radial)<1e-12)
                throw std::runtime_error("Lens inversion diverged");
            x=(xd-dx)/radial; y=(yd-dy)/radial;
        }
        const double norm=std::sqrt(1+x*x+y*y);
        if (!std::isfinite(norm)) throw std::runtime_error("Invalid calibrated ray");
        rays.push_back({float(x/norm),float(y/norm),float(1/norm)});
    }
    return rays;
}

std::vector<Point> point_cloud(const DepthFrame& depth, const std::vector<Point>& rays) {
    if (depth.raw_depth.size()!=rays.size()) throw std::invalid_argument("Depth/ray count mismatch");
    std::vector<Point> result(rays.size());
    for (std::size_t i=0; i<rays.size(); ++i) if (depth.raw_depth[i]>0) {
        const auto range=float(depth.raw_depth[i]/depth_units_per_metre);
        result[i]={rays[i].x*range,rays[i].y*range,rays[i].z*range};
    }
    return result;
}

std::vector<std::uint8_t> rgb8(const RgbFrame& frame) {
    constexpr std::size_t pixels=rgb_width*rgb_height;
    if (frame.i420.size()!=pixels*3/2) throw std::invalid_argument("Invalid I420 frame size");
    std::vector<std::uint8_t> out(pixels*3);
    // BT.601 limited-range conversion. No external video processing dependency.
    for (unsigned y=0; y<rgb_height; ++y) for (unsigned x=0; x<rgb_width; ++x) {
        const std::size_t i=std::size_t(y)*rgb_width+x, chroma=std::size_t(y/2)*(rgb_width/2)+x/2;
        const int c=int(frame.i420[i])-16;
        const int d=int(frame.i420[pixels+chroma])-128, e=int(frame.i420[pixels+pixels/4+chroma])-128;
        // Division avoids implementation-defined right shift of negative values.
        out[i*3]=clip((298*c+409*e+128)/256);
        out[i*3+1]=clip((298*c-100*d-208*e+128)/256);
        out[i*3+2]=clip((298*c+516*d+128)/256);
    }
    return out;
}

std::vector<std::uint16_t> depth_millimetres(const DepthFrame& frame) {
    std::vector<std::uint16_t> out;out.reserve(frame.raw_depth.size());
    for (auto d:frame.raw_depth)
        out.push_back(d>0 ? static_cast<std::uint16_t>(std::lround(d*1000.0/depth_units_per_metre)) : 0);
    return out;
}
} // namespace mars04
