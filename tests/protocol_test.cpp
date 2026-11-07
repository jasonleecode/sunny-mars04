// SPDX-License-Identifier: MIT
#include "mars04/mars04.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
void require(bool condition,const char* what) {if (!condition) throw std::runtime_error(what);}
void rejects(const std::function<void()>& f) {
    bool threw=false;try {f();} catch (const std::exception&) {threw=true;}
    require(threw,"Malformed input was accepted");
}
void put32(std::vector<std::uint8_t>& b,std::size_t p,std::uint32_t v) {
    for (unsigned i=0;i<4;++i) b.at(p+i)=static_cast<std::uint8_t>(v>>(8*i));
}
void putfloat(std::vector<std::uint8_t>& b,std::size_t p,float v) {
    std::uint32_t bits;std::memcpy(&bits,&v,4);put32(b,p,bits);
}
}
int main() {
    try {
        using namespace mars04;
        // Synthetic fixtures contain no camera images, serials or SDK data.
        std::vector<std::uint8_t> info(512);
        put32(info,0,2);put32(info,8,224|(173u<<16));
        put32(info,14,1344|(760u<<16));
        const std::string firmware="M04_V2.44.00.180322";
        std::copy(firmware.begin(),firmware.end(),info.begin()+24);
        const std::string serial="SYNTHETIC-TEST";
        std::copy(serial.begin(),serial.end(),info.begin()+44);
        require(parse_device_info(info).serial==serial,"Serial field offsets");
        auto older=info;put32(older,0,4);
        const std::string older_firmware="M04_V2.43.01.180307";
        std::copy(older_firmware.begin(),older_firmware.end(),older.begin()+24);
        require(parse_device_info(older).firmware==older_firmware,"Legacy type-4 firmware");
        auto mismatched=older;put32(mismatched,0,2);
        rejects([&]{parse_device_info(mismatched);});
        auto invalid=info;invalid[24]='X';rejects([&]{parse_device_info(invalid);});
        invalid=info;invalid[8]=0;rejects([&]{parse_device_info(invalid);});
        rejects([&]{parse_device_info({});});

        std::vector<std::uint8_t> lens_bytes(512);
        putfloat(lens_bytes,0,1);putfloat(lens_bytes,4,1);
        const auto lens=parse_lens(lens_bytes);
        auto rays=make_rays(2,2,lens);
        require(rays[0].z==1 && rays[0].x==0 && rays[0].y==0,"Principal ray");
        require(std::abs(rays[3].x-1/std::sqrt(3.0))<1e-6,"Unit radial ray");
        for (const auto& p:rays) require(std::abs(p.x*p.x+p.y*p.y+p.z*p.z-1)<1e-6,"Ray normalization");
        DepthFrame tiny{{},{5460,10920,0,-1}};
        const auto cloud=point_cloud(tiny,rays);
        require(cloud[0].z==1 && std::abs(std::hypot(cloud[1].x,cloud[1].z)-2)<1e-6,"Metric range conversion");
        require(cloud[2].z==0 && cloud[3].z==0,"Invalid depth filtering");
        require(depth_millimetres(tiny)==std::vector<std::uint16_t>({1000,2000,0,0}),"Millimetre conversion");
        rejects([&]{point_cloud(tiny,{});});
        putfloat(lens_bytes,0,std::numeric_limits<float>::quiet_NaN());
        rejects([&]{parse_lens(lens_bytes);});
        rejects([&]{make_rays(0,2,lens);});

        std::vector<std::uint8_t> depth(depth_packet_bytes);
        constexpr std::size_t dm=depth_width*depth_height*5;
        put32(depth,dm+16,depth_width);put32(depth,dm+32,42);put32(depth,dm+36,1234);
        depth[0]=0xff;depth[1]=0xff;depth[2]=0x54;depth[3]=0x15; // -1 and 5460
        auto frame=parse_depth(depth);
        require(frame.raw_depth[0]==-1 && frame.raw_depth[1]==5460,"Signed little-endian depth");
        require(frame.metadata.sequence==42 && frame.metadata.device_timestamp==1234,"Depth metadata");
        depth.pop_back();rejects([&]{parse_depth(depth);});
        depth.resize(depth_packet_bytes);put32(depth,dm+16,0);rejects([&]{parse_depth(depth);});

        constexpr std::size_t pixels=rgb_width*rgb_height, rgb_bytes=pixels*3/2;
        std::vector<std::uint8_t> rgb(rgb_packet_bytes,128);
        std::fill(rgb.begin(),rgb.begin()+pixels,16);
        put32(rgb,rgb_bytes+16,rgb_width);put32(rgb,rgb_bytes+20,rgb_height);
        auto colour=parse_rgb(rgb);auto converted=rgb8(colour);
        require(converted[0]==0 && converted[1]==0 && converted[2]==0,"Limited-range black");
        colour.i420[0]=235;converted=rgb8(colour);
        require(converted[0]==255 && converted[1]==255 && converted[2]==255,"Limited-range white");
        // BT.601 red sample, and a separate pixel to ensure planar U/V indexing.
        colour.i420[0]=81;colour.i420[pixels]=90;colour.i420[pixels+pixels/4]=240;
        converted=rgb8(colour);
        require(converted[0]>250 && converted[1]<3 && converted[2]<3,"I420 red, U/V order");
        require(converted[6]==0 && converted[7]==0 && converted[8]==0,"Chroma subsampling indexing");
        put32(rgb,rgb_bytes+20,0);rejects([&]{parse_rgb(rgb);});

        std::vector<std::uint8_t> imu(512);imu[0]=1;
        put32(imu,4,0x00554d49);put32(imu,8,5678);putfloat(imu,12,1.25f);
        auto samples=parse_imu(imu);
        require(samples.size()==1 && samples[0].device_timestamp==5678,"Legacy IMU record offsets");
        require(samples[0].channels[0]==1.25f && samples[0].has_nonzero_channels(),"IMU float decoding");
        putfloat(imu,12,0);require(!parse_imu(imu)[0].has_nonzero_channels(),"Zero IMU is not motion evidence");
        imu[0]=12;rejects([&]{parse_imu(imu);});
        imu[0]=1;imu[4]=0;rejects([&]{parse_imu(imu);});
        imu[0]=0;require(parse_imu(imu).empty(),"Empty IMU batch");

        // Bounded randomized malformed responses; useful under ASan/UBSan.
        std::mt19937 random(7304);
        for (unsigned i=0;i<300;++i) {
            std::vector<std::uint8_t> bytes(random()%600);
            for (auto& v:bytes) v=static_cast<std::uint8_t>(random());
            try {parse_device_info(bytes);} catch (const std::exception&) {}
            try {parse_lens(bytes);} catch (const std::exception&) {}
            try {parse_depth(bytes);} catch (const std::exception&) {}
            try {parse_rgb(bytes);} catch (const std::exception&) {}
            try {parse_imu(bytes);} catch (const std::exception&) {}
        }
        std::cout<<"Protocol, geometry, colour, IMU and malformed-input tests passed\n";
        return 0;
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
