// SPDX-License-Identifier: MIT
#include "mars04/mars04.hpp"
#include <png.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
namespace {
std::ofstream output(const fs::path& path, bool binary=false) {
    std::ofstream file;
    file.exceptions(std::ios::failbit|std::ios::badbit);
    file.open(path,binary ? std::ios::out|std::ios::binary : std::ios::out);
    return file;
}
void png(const fs::path& path,unsigned w,unsigned h,png_uint_32 format,const void* data) {
    png_image image{};image.version=PNG_IMAGE_VERSION;
    image.width=w;image.height=h;image.format=format;
    if (!png_image_write_to_file(&image,path.c_str(),0,data,0,nullptr)) {
        const std::string message=image.message;png_image_free(&image);
        throw std::runtime_error("Write PNG: "+message);
    }
    png_image_free(&image);
}
void little_float(std::ostream& file,float value) {
    std::uint32_t bits;std::memcpy(&bits,&value,4);
    for (unsigned shift=0;shift<32;shift+=8) file.put(static_cast<char>((bits>>shift)&255));
}
std::size_t save_cloud(const fs::path& path,const std::vector<mars04::Point>& points) {
    const auto valid=std::count_if(points.begin(),points.end(),[](auto p){return p.z>0;});
    auto file=output(path,true);
    file<<"ply\nformat binary_little_endian 1.0\ncomment units metres; x right y down z forward\nelement vertex "
        <<valid<<"\nproperty float x\nproperty float y\nproperty float z\nend_header\n";
    for (auto p:points) if (p.z>0) {little_float(file,p.x);little_float(file,p.y);little_float(file,p.z);}
    file.close();return static_cast<std::size_t>(valid);
}
std::string prefix(unsigned i) {std::ostringstream s;s<<std::setfill('0')<<std::setw(4)<<i;return s.str();}
unsigned number(const std::string& s,unsigned max) {
    std::size_t end=0;const auto value=std::stoul(s,&end);
    if (end!=s.size() || value<1 || value>max) throw std::invalid_argument("Option number out of range");
    return static_cast<unsigned>(value);
}
struct Stats {
    unsigned frames=0,duplicates=0,gaps=0;
    std::size_t valid_pixels=0;
    double seconds=0;
    mars04::FrameMetadata last{};
    void add(mars04::FrameMetadata m) {
        if (frames) {
            if (m.sequence==last.sequence) ++duplicates;
            else if (m.sequence-last.sequence!=1) ++gaps;
        }
        last=m;++frames;
    }
};
std::string json_stats(const Stats& s) {
    std::ostringstream out;out<<"{\"frames\":"<<s.frames<<",\"seconds\":"<<s.seconds
        <<",\"duplicate_sequences\":"<<s.duplicates<<",\"sequence_discontinuities\":"<<s.gaps
        <<",\"last_valid_depth_pixels\":"<<s.valid_pixels<<"}";
    return out.str();
}
}

int main(int argc,char** argv) {
    try {
        unsigned frames=1,imu_seconds=0;bool check_only=false,info_only=false,imu=false;
        fs::path folder="mars04-capture-"+std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        for (int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            auto next=[&]() -> std::string {if (++i==argc) throw std::invalid_argument("Missing value for "+arg);return argv[i];};
            if (arg=="--output") folder=next();
            else if (arg=="--frames") frames=number(next(),10000);
            else if (arg=="--check") {frames=number(next(),10000);check_only=true;}
            else if (arg=="--info") info_only=true;
            else if (arg=="--imu") imu=true;
            else if (arg=="--imu-only") {imu_seconds=number(next(),3600);imu=true;}
            else if (arg=="--help" || arg=="-h") {
                std::cout<<"mars04-capture [--output NEW_DIRECTORY] [--frames N | --check N] [--imu]\n"
                    <<"mars04-capture --info\nmars04-capture --imu-only SECONDS [--output NEW_DIRECTORY]\n\n"
                    <<"RGB PNG + raw I420, depth millimetres PNG, metric PLY and JSON metadata.\n"
                    <<"--check validates frames without saving images. --imu adds experimental\n"
                    <<"raw IMU diagnostics (units/sensor validity NOT verified) in imu.csv.\n";
                return 0;
            } else throw std::invalid_argument("Unknown option: "+arg);
        }
        if (!info_only && (!check_only || imu) && !fs::create_directories(folder))
            throw std::runtime_error("Output directory already exists: "+folder.string());
        mars04::Camera camera;
        std::cout<<"Firmware: "<<camera.info().firmware<<"\nSerial: "<<camera.info().serial<<std::endl;
        if (info_only) return 0;
        std::atomic<bool> stop{false};
        std::size_t imu_samples=0,imu_nonzero=0,imu_errors=0;
        std::future<void> imu_future;
        if (imu) imu_future=std::async(std::launch::async,[&] {
            auto csv=output(folder/"imu.csv");
            csv<<"host_steady_ns,device_timestamp,channel_0,channel_1,channel_2,channel_3,channel_4,channel_5,channel_6,channel_7,channel_8,has_nonzero_channels\n";
            csv<<std::setprecision(9);
            const auto until=Clock::now()+std::chrono::seconds(imu_seconds);
            while (!stop && (!imu_seconds || Clock::now()<until)) {
                std::vector<mars04::ImuSample> batch;
                try {batch=camera.read_imu();}
                catch (const std::runtime_error&) {++imu_errors;}
                const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
                for (const auto& sample:batch) {
                    csv<<now<<','<<sample.device_timestamp;
                    for (auto v:sample.channels) csv<<','<<v;
                    csv<<','<<sample.has_nonzero_channels()<<'\n';
                    ++imu_samples;imu_nonzero+=sample.has_nonzero_channels();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            csv.close();
        });
        Stats depth_stats,rgb_stats;
        try {
            if (!imu_seconds) {
                const auto rays=check_only ? std::vector<mars04::Point>{} : mars04::make_rays(mars04::depth_width,mars04::depth_height,camera.lens());
                auto depth_future=std::async(std::launch::async,[&] {
                    Stats stats;const auto start=Clock::now();
                    std::ofstream csv;if (!check_only) {csv=output(folder/"depth.csv");csv<<"frame,sequence,device_timestamp\n";}
                    for (unsigned i=0;i<frames;++i) {
                        const auto depth=camera.read_depth();stats.add(depth.metadata);
                        stats.valid_pixels=static_cast<std::size_t>(std::count_if(depth.raw_depth.begin(),depth.raw_depth.end(),[](auto d){return d>0;}));
                        if (!check_only) {
                            csv<<i<<','<<depth.metadata.sequence<<','<<depth.metadata.device_timestamp<<'\n';
                            const auto mm=mars04::depth_millimetres(depth);
                            png(folder/(prefix(i)+"-depth-mm.png"),mars04::depth_width,mars04::depth_height,PNG_FORMAT_LINEAR_Y,mm.data());
                            save_cloud(folder/(prefix(i)+".ply"),mars04::point_cloud(depth,rays));
                        }
                    }
                    if (csv.is_open()) csv.close();
                    stats.seconds=std::chrono::duration<double>(Clock::now()-start).count();return stats;
                });
                auto rgb_future=std::async(std::launch::async,[&] {
                    Stats stats;const auto start=Clock::now();
                    std::ofstream csv;if (!check_only) {csv=output(folder/"rgb.csv");csv<<"frame,sequence,device_timestamp\n";}
                    for (unsigned i=0;i<frames;++i) {
                        const auto rgb=camera.read_rgb();stats.add(rgb.metadata);
                        if (!check_only) {
                            csv<<i<<','<<rgb.metadata.sequence<<','<<rgb.metadata.device_timestamp<<'\n';
                            auto raw=output(folder/(prefix(i)+".yuv"),true);
                            raw.write(reinterpret_cast<const char*>(rgb.i420.data()),static_cast<std::streamsize>(rgb.i420.size()));raw.close();
                            const auto image=mars04::rgb8(rgb);
                            png(folder/(prefix(i)+"-rgb.png"),mars04::rgb_width,mars04::rgb_height,PNG_FORMAT_RGB,image.data());
                        }
                    }
                    if (csv.is_open()) csv.close();
                    stats.seconds=std::chrono::duration<double>(Clock::now()-start).count();return stats;
                });
                depth_stats=depth_future.get();rgb_stats=rgb_future.get();
            }
            if (!imu_seconds) stop=true;
            if (imu_future.valid()) imu_future.get();
        } catch (...) {stop=true;if (imu_future.valid()) imu_future.wait();throw;}
        std::ostringstream json;
        json<<std::setprecision(9)<<"{\n  \"firmware\": "<<std::quoted(camera.info().firmware)
            <<",\n  \"serial\": "<<std::quoted(camera.info().serial)<<",\n  \"lens\": [";
        for (std::size_t i=0;i<camera.lens().size();++i) json<<(i ? "," : "")<<camera.lens()[i];
        json<<"],\n  \"depth\": "<<json_stats(depth_stats)<<",\n  \"rgb\": "<<json_stats(rgb_stats)
            <<",\n  \"rgb_format\": \"I420 / BT.601 limited range\",\n  \"depth_units_per_metre\": 5460,"
            <<"\n  \"synchronized_streams\": false,\n  \"imu\": {\"enabled\": "<<(imu ? "true" : "false")
            <<", \"samples\": "<<imu_samples<<", \"nonzero_samples\": "<<imu_nonzero
            <<", \"query_or_parse_errors\": "<<imu_errors<<", \"motion_data_validated\": false, \"units\": \"unknown\"}\n}\n";
        std::cout<<json.str();
        if (!check_only || imu) {auto file=output(folder/"capture.json");file<<json.str();file.close();std::cout<<"Saved: "<<folder<<'\n';}
        if (imu) std::cerr<<"IMU diagnostics only: sensor validity, units and axes are unverified. Nonzero values may be firmware garbage.\n";
        return 0;
    } catch (const std::exception& e) {std::cerr<<"mars04-capture: "<<e.what()<<'\n';return 1;}
}
