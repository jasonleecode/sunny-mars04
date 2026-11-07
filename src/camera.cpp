// SPDX-License-Identifier: MIT
#include "mars04/mars04.hpp"
#include <libusb.h>
#include <stdexcept>
#include <utility>
#include <mutex>

namespace mars04 {
namespace {
void check(int result, const char* operation) {
    if (result<0) throw std::runtime_error(std::string(operation)+": "+libusb_error_name(result));
}
}
struct Camera::Impl {
    libusb_context* context=nullptr;
    libusb_device_handle* handle=nullptr;
    bool claimed=false;
    unsigned timeout;
    DeviceInfo device;
    Lens calibration{};
    std::mutex control_mutex;
    explicit Impl(unsigned milliseconds):timeout(milliseconds) {}
    ~Impl() {
        if (handle) {
            if (claimed) libusb_release_interface(handle,0);
            libusb_close(handle);
        }
        if (context) libusb_exit(context);
    }
    std::vector<std::uint8_t> query(std::uint32_t opcode) {
        std::lock_guard<std::mutex> lock(control_mutex);
        if (opcode!=0x10050 && opcode!=0x10015 && opcode!=0x10009) throw std::logic_error("Unverified USB query");
        std::array<unsigned char,4> command{{static_cast<unsigned char>(opcode),
            static_cast<unsigned char>(opcode>>8),static_cast<unsigned char>(opcode>>16),
            static_cast<unsigned char>(opcode>>24)}};
        auto n=libusb_control_transfer(handle,0x40,0,0,0,command.data(),4,timeout);
        check(n,"Send metadata query");
        if (n!=4) throw std::runtime_error("Short metadata command");
        // This firmware stalls requests with some other buffer lengths.
        std::vector<std::uint8_t> reply(512);
        n=libusb_control_transfer(handle,0xc0,0,opcode==0x10009 ? 0x99 : 0,0,reply.data(),512,timeout);
        check(n,"Read metadata reply");
        reply.resize(static_cast<std::size_t>(n));
        return reply;
    }
    std::vector<std::uint8_t> read(unsigned char endpoint, std::size_t size) {
        std::vector<std::uint8_t> bytes(size);int actual=0;
        const int result=libusb_bulk_transfer(handle,endpoint,bytes.data(),static_cast<int>(size),&actual,timeout);
        check(result,"Read USB image");
        if (static_cast<std::size_t>(actual)!=size)
            throw std::runtime_error("Short USB image; reopen camera to reset frame boundaries");
        return bytes;
    }
};

Camera::Camera(unsigned timeout_ms):impl_(std::make_unique<Impl>(timeout_ms)) {
    if (!timeout_ms) throw std::invalid_argument("USB timeout must be positive");
    check(libusb_init(&impl_->context),"Initialize libusb");
    impl_->handle=libusb_open_device_with_vid_pid(impl_->context,vendor_id,product_id);
    if (!impl_->handle) throw std::runtime_error("Mars04 040e:4d35 not found or USB permission denied");
    // Never detach an unrelated kernel driver automatically.
    check(libusb_claim_interface(impl_->handle,0),"Claim camera interface 0");
    impl_->claimed=true;
    impl_->device=parse_device_info(impl_->query(0x10050));
    switch (libusb_get_device_speed(libusb_get_device(impl_->handle))) {
    case LIBUSB_SPEED_LOW: impl_->device.usb_speed_mbps=1;break;
    case LIBUSB_SPEED_FULL: impl_->device.usb_speed_mbps=12;break;
    case LIBUSB_SPEED_HIGH: impl_->device.usb_speed_mbps=480;break;
    case LIBUSB_SPEED_SUPER: impl_->device.usb_speed_mbps=5000;break;
    case LIBUSB_SPEED_SUPER_PLUS: impl_->device.usb_speed_mbps=10000;break;
    default: break;
    }
    impl_->calibration=parse_lens(impl_->query(0x10015));
    check(libusb_reset_device(impl_->handle),"Reset camera USB queues");
}
Camera::~Camera()=default;
Camera::Camera(Camera&&) noexcept=default;
Camera& Camera::operator=(Camera&&) noexcept=default;
const DeviceInfo& Camera::info() const { return impl_->device; }
const Lens& Camera::lens() const { return impl_->calibration; }
DepthFrame Camera::read_depth() { return parse_depth(impl_->read(0x82,depth_packet_bytes)); }
RgbFrame Camera::read_rgb() { return parse_rgb(impl_->read(0x83,rgb_packet_bytes)); }
std::vector<ImuSample> Camera::read_imu() { return parse_imu(impl_->query(0x10009)); }
} // namespace mars04
