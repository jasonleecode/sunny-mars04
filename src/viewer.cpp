// SPDX-License-Identifier: MIT
#include "mars04/mars04.hpp"
#include <SDL.h>
#include <cairo.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace {
using Clock=std::chrono::steady_clock;
namespace fs=std::filesystem;
constexpr int screen_w=1440,screen_h=900;
constexpr double pi=3.141592653589793;
struct Rect {
    int x,y,w,h;
    bool contains(int px,int py) const {return px>=x && px<x+w && py>=y && py<y+h;}
};
const Rect rgb_card{20,108,470,346}, depth_card{20,470,470,354};
const Rect cloud_card{506,108,594,716}, imu_card{1116,108,304,716};
const Rect cloud_view{522,184,562,542};
const Rect connect_button{884,28,154,42}, freeze_button{1050,28,112,42};
const Rect reset_button{1174,28,110,42}, save_button{1296,28,124,42};

std::uint32_t range_colour(double metres) {
    // Near: cyan, middle: green, far: warm orange. Invalid is dark navy.
    const double t=std::clamp(metres/4.0,0.0,1.0);
    const auto r=unsigned(std::clamp(25+230*t,0.0,255.0));
    const auto g=unsigned(std::clamp(205-70*t+45*std::sin(t*pi),0.0,255.0));
    const auto b=unsigned(std::clamp(240-210*t,0.0,255.0));
    return 0xff000000u|(r<<16)|(g<<8)|b;
}
struct Image {
    int w=0,h=0;
    std::vector<std::uint32_t> pixels;
    mars04::FrameMetadata metadata;
};
std::shared_ptr<Image> colour_preview(const mars04::RgbFrame& frame) {
    auto out=std::make_shared<Image>();out->w=672;out->h=380;
    out->pixels.resize(std::size_t(out->w)*out->h);out->metadata=frame.metadata;
    constexpr std::size_t plane=mars04::rgb_width*mars04::rgb_height;
    const auto clip=[](int n){return std::uint32_t(std::clamp(n,0,255));};
    for (int y=0;y<out->h;++y) for (int x=0;x<out->w;++x) {
        const auto dst=std::size_t(y)*out->w+x;
        const auto uv=std::size_t(y)*out->w+x;
        const int c=int(frame.i420[std::size_t(y*2)*mars04::rgb_width+std::size_t(x*2)])-16;
        const int d=int(frame.i420[plane+uv])-128,e=int(frame.i420[plane+plane/4+uv])-128;
        out->pixels[dst]=0xff000000u|(clip((298*c+409*e+128)/256)<<16)|
            (clip((298*c-100*d-208*e+128)/256)<<8)|clip((298*c+516*d+128)/256);
    }
    return out;
}
std::shared_ptr<Image> depth_preview(const mars04::DepthFrame& frame) {
    static const auto palette=[] {
        std::array<std::uint32_t,32768> colours{};colours[0]=0xff101a2bu;
        for (std::size_t i=1;i<colours.size();++i) colours[i]=range_colour(double(i)/mars04::depth_units_per_metre);
        return colours;
    }();
    auto out=std::make_shared<Image>();out->w=int(mars04::depth_width);out->h=int(mars04::depth_height);
    out->pixels.reserve(frame.raw_depth.size());out->metadata=frame.metadata;
    for (auto d:frame.raw_depth) out->pixels.push_back(palette[d>0 ? std::size_t(d) : 0]);
    return out;
}
struct State {
    std::string status="Connecting to camera...",firmware,error;
    bool connected=false;
    std::shared_ptr<const Image> rgb,depth;
    std::shared_ptr<const std::vector<mars04::Point>> cloud;
    std::uint64_t rgb_count=0,depth_count=0,imu_count=0,imu_errors=0,imu_nonzero=0;
    std::size_t valid_points=0;
    mars04::ImuSample imu;
    std::deque<std::array<float,9>> history;
    Clock::time_point rgb_arrival{},depth_arrival{},imu_arrival{};
    double rgb_read_seconds=0,rgb_convert_seconds=0,depth_read_seconds=0,depth_convert_seconds=0;
};
class Acquisition {
    std::mutex mutex_;
    State state_;
    std::atomic<bool> wanted_{true},quit_{false};
    std::thread thread_;
    void session() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.status="Connecting to camera...";state_.error.clear();state_.connected=false;
        }
        mars04::Camera camera;
        const auto rays=mars04::make_rays(mars04::depth_width,mars04::depth_height,camera.lens());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_=State{};state_.status="Streaming";state_.connected=true;state_.firmware=camera.info().firmware;
        }
        std::atomic<bool> failed{false};
        auto active=[&]{return wanted_ && !quit_ && !failed;};
        auto error=[&](const std::string& message) {
            failed=true;
            std::lock_guard<std::mutex> lock(mutex_);state_.error=message;
        };
        std::future<void> depth,rgb,imu;
        // Signal existing readers before futures join during stack unwinding.
        struct StopWorkers {std::atomic<bool>& failed;~StopWorkers(){failed=true;}} stop_workers{failed};
        depth=std::async(std::launch::async,[&] {
            try {
                while (active()) {
                    const auto begin=Clock::now();const auto frame=camera.read_depth();
                    const auto received=Clock::now();
                    auto image=depth_preview(frame);
                    auto points=std::make_shared<std::vector<mars04::Point>>(mars04::point_cloud(frame,rays));
                    const auto valid=std::count_if(points->begin(),points->end(),[](auto p){return p.z>0;});
                    std::lock_guard<std::mutex> lock(mutex_);
                    state_.depth=std::move(image);state_.cloud=std::move(points);
                    state_.valid_points=static_cast<std::size_t>(valid);++state_.depth_count;state_.depth_arrival=Clock::now();
                    state_.depth_read_seconds+=std::chrono::duration<double>(received-begin).count();
                    state_.depth_convert_seconds+=std::chrono::duration<double>(Clock::now()-received).count();
                }
            } catch (const std::exception& e) {error(e.what());}
        });
        rgb=std::async(std::launch::async,[&] {
            try {
                while (active()) {
                    const auto begin=Clock::now();auto frame=camera.read_rgb();
                    const auto received=Clock::now();auto image=colour_preview(frame);
                    std::lock_guard<std::mutex> lock(mutex_);state_.rgb=std::move(image);
                    ++state_.rgb_count;state_.rgb_arrival=Clock::now();
                    state_.rgb_read_seconds+=std::chrono::duration<double>(received-begin).count();
                    state_.rgb_convert_seconds+=std::chrono::duration<double>(Clock::now()-received).count();
                }
            } catch (const std::exception& e) {error(e.what());}
        });
        imu=std::async(std::launch::async,[&] {
            try {
                while (active()) {
                    try {
                        const auto batch=camera.read_imu();
                        std::lock_guard<std::mutex> lock(mutex_);
                        for (const auto& sample:batch) {
                            state_.imu=sample;++state_.imu_count;state_.imu_nonzero+=sample.has_nonzero_channels();
                            state_.history.push_back(sample.channels);
                            if (state_.history.size()>150) state_.history.pop_front();
                            state_.imu_arrival=Clock::now();
                        }
                    } catch (const std::exception&) {std::lock_guard<std::mutex> lock(mutex_);++state_.imu_errors;}
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            } catch (const std::exception& e) {error(e.what());}
        });
        depth.get();rgb.get();imu.get();
        std::lock_guard<std::mutex> lock(mutex_);state_.connected=false;
        if (failed) {state_.status="Camera error - reconnect to retry";wanted_=false;}
        else state_.status="Disconnected - last received frames shown";
    }
    void run() {
        while (!quit_) {
            if (!wanted_) {std::this_thread::sleep_for(std::chrono::milliseconds(50));continue;}
            try {session();}
            catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(mutex_);state_.error=e.what();
                state_.status="Camera unavailable - reconnect to retry";state_.connected=false;wanted_=false;
            }
        }
    }
public:
    Acquisition():thread_([this]{run();}) {}
    ~Acquisition() {quit_=true;wanted_=false;if (thread_.joinable()) thread_.join();}
    State snapshot() {std::lock_guard<std::mutex> lock(mutex_);return state_;}
    void toggle() {
        wanted_=!wanted_;
        std::lock_guard<std::mutex> lock(mutex_);
        state_.status=wanted_ ? "Connecting to camera..." : "Disconnecting...";
    }
    bool wanted() const {return wanted_;}
};

void source(cairo_t* cr,unsigned colour,double alpha=1) {
    cairo_set_source_rgba(cr,double((colour>>16)&255)/255,double((colour>>8)&255)/255,double(colour&255)/255,alpha);
}
void rounded(cairo_t* cr,Rect r,double radius=12) {
    cairo_new_sub_path(cr);
    cairo_arc(cr,r.x+r.w-radius,r.y+radius,radius,-pi/2,0);
    cairo_arc(cr,r.x+r.w-radius,r.y+r.h-radius,radius,0,pi/2);
    cairo_arc(cr,r.x+radius,r.y+r.h-radius,radius,pi/2,pi);
    cairo_arc(cr,r.x+radius,r.y+radius,radius,pi,3*pi/2);cairo_close_path(cr);
}
void label(cairo_t* cr,double x,double y,const std::string& text,double size=14,unsigned colour=0xcbd5e1,bool bold=false) {
    cairo_select_font_face(cr,"DejaVu Sans",CAIRO_FONT_SLANT_NORMAL,bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr,size);source(cr,colour);cairo_move_to(cr,x,y);cairo_show_text(cr,text.c_str());
}
void panel(cairo_t* cr,Rect r) {
    rounded(cr,r);source(cr,0x111d2f);cairo_fill_preserve(cr);
    source(cr,0x25354a);cairo_set_line_width(cr,1);cairo_stroke(cr);
}
void button(cairo_t* cr,Rect r,const std::string& text,bool accent=false) {
    rounded(cr,r,8);source(cr,accent ? 0x18524f : 0x1a2b40);cairo_fill_preserve(cr);
    source(cr,accent ? 0x337d74 : 0x35465b);cairo_stroke(cr);
    label(cr,r.x+14,r.y+26,text,14,accent ? 0x9ff5dc : 0xe2e8f0,true);
}
void image(cairo_t* cr,const std::shared_ptr<const Image>& img,Rect r) {
    source(cr,0x080f1c);cairo_rectangle(cr,r.x,r.y,r.w,r.h);cairo_fill(cr);
    if (!img) {label(cr,r.x+20,r.y+r.h/2,"Waiting for frames...",16,0x64748b);return;}
    const auto scale=std::min(double(r.w)/img->w,double(r.h)/img->h);
    auto* surface=cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(const_cast<std::uint32_t*>(img->pixels.data())),
        CAIRO_FORMAT_ARGB32,img->w,img->h,img->w*4);
    cairo_save(cr);cairo_translate(cr,r.x+(r.w-img->w*scale)/2,r.y+(r.h-img->h*scale)/2);
    cairo_scale(cr,scale,scale);cairo_set_source_surface(cr,surface,0,0);
    cairo_pattern_set_filter(cairo_get_source(cr),CAIRO_FILTER_BILINEAR);cairo_paint(cr);cairo_restore(cr);
    cairo_surface_destroy(surface);
}
std::string counter(const std::shared_ptr<const Image>& image) {
    if (!image) return "No frame yet";
    return "SEQ "+std::to_string(image->metadata.sequence)+"    DEVICE TIME "+std::to_string(image->metadata.device_timestamp);
}
std::string fixed(double value,int digits=1) {std::ostringstream s;s<<std::fixed<<std::setprecision(digits)<<value;return s.str();}
std::string raw_value(float value) {
    std::ostringstream s;
    if (value!=0 && (std::abs(value)<.0001f || std::abs(value)>9999)) s<<std::scientific<<std::setprecision(3)<<value;
    else s<<std::fixed<<std::setprecision(5)<<value;
    return s.str();
}

struct View {
    double yaw=0,pitch=0,distance=3.0;
    double pan_x=0,pan_y=0;
    bool full=true;
    void reset() {yaw=0;pitch=0;distance=3.0;pan_x=0;pan_y=0;}
};
class CloudRenderer {
    std::vector<std::uint32_t> pixels_;
    std::vector<float> zbuffer_;
    cairo_surface_t* surface_=nullptr;
public:
    CloudRenderer():pixels_(std::size_t(cloud_view.w)*cloud_view.h),zbuffer_(pixels_.size()) {
        surface_=cairo_image_surface_create_for_data(reinterpret_cast<unsigned char*>(pixels_.data()),
            CAIRO_FORMAT_ARGB32,cloud_view.w,cloud_view.h,cloud_view.w*4);
    }
    ~CloudRenderer() {cairo_surface_destroy(surface_);}
    void draw(cairo_t* cr,const State& state,const View& view) {
        std::fill(pixels_.begin(),pixels_.end(),0xff080f1cu);
        std::fill(zbuffer_.begin(),zbuffer_.end(),std::numeric_limits<float>::infinity());
        if (state.cloud) {
            double cx=0,cy=0,cz=0;std::size_t valid=0;
            for (auto p:*state.cloud) if (p.z>0) {cx+=p.x;cy+=p.y;cz+=p.z;++valid;}
            if (valid) {cx/=double(valid);cy/=double(valid);cz/=double(valid);}
            const auto sy=std::sin(view.yaw),co=std::cos(view.yaw),sp=std::sin(view.pitch),cp=std::cos(view.pitch);
            const auto step=view.full ? std::size_t(1) : std::size_t(2);
            for (std::size_t i=0;i<state.cloud->size();i+=step) {
                const auto p=(*state.cloud)[i];if (p.z<=0) continue;
                const double x=p.x-cx,y=p.y-cy,z=p.z-cz;
                const double rx=co*x+sy*z, rz=-sy*x+co*z;
                const double ry=cp*y-sp*rz, camera_z=sp*y+cp*rz+view.distance;
                if (camera_z<.08) continue;
                const double px=cloud_view.w/2.0+rx*470/camera_z+view.pan_x;
                const double py=cloud_view.h/2.0+ry*470/camera_z+view.pan_y;
                if (!std::isfinite(px) || !std::isfinite(py) || px<0 || py<0 || px>=cloud_view.w-1 || py>=cloud_view.h-1) continue;
                const int ix=int(px),iy=int(py);
                // Organized points and depth preview share this acquisition frame.
                // Reuse its range colour rather than sqrt + sin per rendered point.
                const auto colour=state.depth && i<state.depth->pixels.size() ? state.depth->pixels[i] : 0xff53d8c4u;
                for (int dy=0;dy<2;++dy) for (int dx=0;dx<2;++dx) {
                    const auto j=std::size_t(iy+dy)*cloud_view.w+std::size_t(ix+dx);
                    if (camera_z<zbuffer_[j]) {zbuffer_[j]=float(camera_z);pixels_[j]=colour;}
                }
            }
        }
        cairo_surface_mark_dirty(surface_);cairo_set_source_surface(cr,surface_,cloud_view.x,cloud_view.y);cairo_paint(cr);
        if (!state.cloud) label(cr,cloud_view.x+125,cloud_view.y+260,"Waiting for depth...",16,0x64748b);
        // Camera-coordinate orientation indicator, transformed with the cloud.
        const double ox=cloud_view.x+42,oy=cloud_view.y+cloud_view.h-40;
        const std::array<std::array<double,3>,3> axes{{{{1,0,0}},{{0,1,0}},{{0,0,1}}}};
        const std::array<unsigned,3> colours{{0xff7f7f,0x8be9b4,0x75b9ff}};
        const std::array<const char*,3> names{{"X","Y","Z"}};
        for (std::size_t i=0;i<3;++i) {
            const auto a=axes[i];const double rx=std::cos(view.yaw)*a[0]+std::sin(view.yaw)*a[2];
            const double rz=-std::sin(view.yaw)*a[0]+std::cos(view.yaw)*a[2];
            const double ry=std::cos(view.pitch)*a[1]-std::sin(view.pitch)*rz;
            source(cr,colours[i]);cairo_set_line_width(cr,2);cairo_move_to(cr,ox,oy);cairo_line_to(cr,ox+rx*26,oy+ry*26);cairo_stroke(cr);
            label(cr,ox+rx*32-4,oy+ry*32+4,names[i],11,colours[i]);
        }
    }
};

void sparkline(cairo_t* cr,Rect r,const State& state,int group) {
    source(cr,0x0b1423);cairo_rectangle(cr,r.x,r.y,r.w,r.h);cairo_fill(cr);
    double peak=.001;
    for (const auto& row:state.history) for (int j=0;j<3;++j) {
        const auto v=row[std::size_t(group*3+j)];
        if (std::isfinite(v) && std::abs(v)<10000) peak=std::max(peak,std::abs(double(v)));
    }
    const std::array<unsigned,3> colours{{0x53d8c4,0x79aaff,0xf1bb6b}};
    cairo_save(cr);cairo_rectangle(cr,r.x,r.y,r.w,r.h);cairo_clip(cr);
    source(cr,0x26364c);cairo_set_line_width(cr,1);cairo_move_to(cr,r.x,r.y+r.h/2.0);cairo_line_to(cr,r.x+r.w,r.y+r.h/2.0);cairo_stroke(cr);
    for (int j=0;j<3;++j) {
        bool started=false;source(cr,colours[std::size_t(j)]);cairo_set_line_width(cr,1.25);
        for (std::size_t i=0;i<state.history.size();++i) {
            const auto v=state.history[i][std::size_t(group*3+j)];
            if (!std::isfinite(v) || std::abs(v)>=10000) {started=false;continue;}
            const auto x=r.x+double(i)*r.w/149,y=r.y+r.h/2.0-double(v)/peak*(r.h/2.0-4);
            if (started) cairo_line_to(cr,x,y);else {cairo_move_to(cr,x,y);started=true;}
        }
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

void draw(cairo_t* cr,const State& state,const State& display,const View& view,CloudRenderer& cloud,
          bool frozen,bool wanted,double rgb_fps,double depth_fps,const std::string& notice) {
    source(cr,0x080f1c);cairo_paint(cr);
    label(cr,22,43,"MARS04",29,0xf1f5f9,true);label(cr,175,43,"/  CAMERA LAB",20,0x86a6be);
    label(cr,23,71,"RGB  /  Depth  /  Interactive XYZ  /  IMU diagnostics",14,0x8299af);
    source(cr,state.connected ? 0x58dbb1 : 0xefb86c);cairo_arc(cr,642,38,4,0,2*pi);cairo_fill(cr);
    label(cr,655,43,state.connected ? "LIVE USB 2.0" : "OFFLINE",13,state.connected ? 0x7ee5c3 : 0xf3c586,true);
    label(cr,641,68,"RGB "+fixed(rgb_fps)+" / Depth "+fixed(depth_fps)+" fps",12,0x94a9be);
    button(cr,connect_button,wanted ? "Disconnect" : "Reconnect",true);
    button(cr,freeze_button,frozen ? "Resume" : "Freeze");
    button(cr,reset_button,"Reset 3D");button(cr,save_button,"Save view");
    panel(cr,rgb_card);panel(cr,depth_card);panel(cr,cloud_card);panel(cr,imu_card);
    label(cr,38,139,"RGB IMAGE",15,0xe5edf7,true);label(cr,311,139,"1344 x 760 / I420",12,0x8aa1b9);
    image(cr,display.rgb,{36,153,438,264});label(cr,38,438,counter(display.rgb),11,0x8299af);
    label(cr,38,501,"RADIAL DEPTH",15,0xe5edf7,true);label(cr,318,501,"224 x 172",12,0x8aa1b9);
    image(cr,display.depth,{36,515,438,266});label(cr,38,806,counter(display.depth),11,0x8299af);
    label(cr,524,139,"XYZ POINT CLOUD",15,0xe5edf7,true);
    label(cr,524,165,std::to_string(display.valid_points)+" valid points  /  metres  /  range colour",13,0x8299af);
    cloud.draw(cr,display,view);
    label(cr,524,751,"Drag: rotate   Right-drag: pan   Wheel: zoom",13,0x9fb3c8);
    label(cr,524,774,"R: reset   D: "+std::string(view.full ? "full density" : "half density"),13,0x8299af);
    for (int i=0;i<190;++i) {source(cr,range_colour(double(i)*4/189));cairo_rectangle(cr,524+i,791,1,9);cairo_fill(cr);}
    label(cr,729,801,"0 - 4 m   /   not RGB-aligned",12,0x8299af);

    label(cr,1134,139,"IMU / RAW CHANNELS",15,0xe5edf7,true);
    rounded(cr,{1132,154,272,34},6);source(cr,0x493722);cairo_fill(cr);
    label(cr,1144,176,"MOTION NOT VALIDATED",13,0xf4c37b,true);
    label(cr,1134,211,"Units and axes unknown",13,0x97a9be);
    label(cr,1134,234,"DEVICE TIME",11,0x728ba3);
    label(cr,1252,234,display.imu_count ? std::to_string(display.imu.device_timestamp) : "--",12,0xcbd5e1);
    label(cr,1134,258,"Samples "+std::to_string(display.imu_count)+" / errors "+std::to_string(display.imu_errors),12,0x8aa1b9);
    for (int i=0;i<9;++i) {
        const int y=294+i*26;
        label(cr,1134,y,"CH "+std::to_string(i),12,0x8299af);
        const auto value=display.imu.channels[std::size_t(i)];
        label(cr,1222,y,display.imu_count ? raw_value(value) : "--",15,std::abs(value)>10000 ? 0xff8990 : 0xe2ecf7);
    }
    for (int group=0;group<3;++group) {
        label(cr,1134,550+group*67,"CH "+std::to_string(group*3)+" / "+std::to_string(group*3+1)+" / "+std::to_string(group*3+2),10,0x8299af);
        sparkline(cr,{1134,558+group*67,268,43},display,group);
    }
    label(cr,1134,763,"Zero / nonzero is not validity.",12,0xf4c37b);
    label(cr,1134,785,"Extreme values omitted from plots.",11,0x8aa1b9);
    label(cr,1134,805,"Raw units; no sensor fusion.",11,0x8aa1b9);

    const std::string status=frozen ? "DISPLAY FROZEN - acquisition continues" : state.status;
    label(cr,23,852,status,14,frozen ? 0xf4c37b : 0xa8c3d6,true);
    label(cr,23,875,"Device timestamps are raw counters; streams are not synchronized.  Space: freeze / resume   Esc: quit",12,0x738da5);
    if (!state.error.empty()) label(cr,540,852,state.error.substr(0,108),12,0xff9292);
    else if (!notice.empty()) label(cr,540,852,notice.substr(0,110),12,0x7ee5c3);
}

std::string save_view(cairo_surface_t* surface,const State& state) {
    const auto* home=std::getenv("HOME");
    const auto tick=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const fs::path folder=fs::path(home ? home : ".")/"Mars04-captures"/("gui-"+std::to_string(tick));
    fs::create_directories(folder);
    if (cairo_surface_write_to_png(surface,(folder/"view.png").c_str())!=CAIRO_STATUS_SUCCESS)
        throw std::runtime_error("Cannot save screenshot");
    if (state.cloud) {
        std::ofstream ply(folder/"cloud.ply");ply.exceptions(std::ios::failbit|std::ios::badbit);
        ply<<"ply\nformat ascii 1.0\ncomment units metres\nelement vertex "<<state.valid_points
            <<"\nproperty float x\nproperty float y\nproperty float z\nend_header\n"<<std::setprecision(8);
        for (auto p:*state.cloud) if (p.z>0) ply<<p.x<<' '<<p.y<<' '<<p.z<<'\n';
        ply.close();
    }
    return "Saved view + cloud: "+folder.filename().string();
}
unsigned positive(const std::string& s) {
    std::size_t end=0;const auto n=std::stoul(s,&end);
    if (end!=s.size() || !n || n>3600) throw std::invalid_argument("Seconds must be in 1..3600");
    return static_cast<unsigned>(n);
}
struct Sdl {
    SDL_Window* window=nullptr;SDL_Renderer* renderer=nullptr;SDL_Texture* texture=nullptr;
    Sdl() {
        if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)<0) throw std::runtime_error(SDL_GetError());
    }
    ~Sdl() {if (texture) SDL_DestroyTexture(texture);if (renderer) SDL_DestroyRenderer(renderer);if (window) SDL_DestroyWindow(window);SDL_Quit();}
};
}

int main(int argc,char** argv) {
    try {
        unsigned run_seconds=0;fs::path screenshot,report;
        for (int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            auto next=[&]() -> std::string {if (++i==argc) throw std::invalid_argument("Missing value for "+arg);return argv[i];};
            if (arg=="--run-seconds") run_seconds=positive(next());
            else if (arg=="--screenshot") screenshot=next();
            else if (arg=="--report") report=next();
            else if (arg=="--help" || arg=="-h") {
                std::cout<<"mars04-viewer [--run-seconds N] [--screenshot FILE.png] [--report FILE.json]\n"
                    <<"Auto-connects to Sunny Mars04. Drag cloud to rotate, right-drag to pan,\n"
                    <<"wheel to zoom. R resets 3D, D changes density, Space freezes display, Esc quits.\n"
                    <<"IMU values are raw diagnostics, NOT validated motion measurements.\n";return 0;
            } else throw std::invalid_argument("Unknown option: "+arg);
        }
        Sdl sdl;
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"linear");
        sdl.window=SDL_CreateWindow("Mars04 Camera Lab",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,screen_w,screen_h,SDL_WINDOW_RESIZABLE);
        if (!sdl.window) throw std::runtime_error(SDL_GetError());
        SDL_SetWindowMinimumSize(sdl.window,1000,625);
        // CPU renderer avoids competing with the N1 video player's GPU context.
        sdl.renderer=SDL_CreateRenderer(sdl.window,-1,SDL_RENDERER_SOFTWARE);
        if (!sdl.renderer) throw std::runtime_error(SDL_GetError());
        SDL_RenderSetLogicalSize(sdl.renderer,screen_w,screen_h);
        sdl.texture=SDL_CreateTexture(sdl.renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,screen_w,screen_h);
        if (!sdl.texture) throw std::runtime_error(SDL_GetError());
        using SurfacePtr=std::unique_ptr<cairo_surface_t,decltype(&cairo_surface_destroy)>;
        SurfacePtr surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32,screen_w,screen_h),cairo_surface_destroy);
        if (cairo_surface_status(surface.get())!=CAIRO_STATUS_SUCCESS) throw std::runtime_error("Cannot allocate UI surface");
        using CairoPtr=std::unique_ptr<cairo_t,decltype(&cairo_destroy)>;
        CairoPtr cr(cairo_create(surface.get()),cairo_destroy);
        CloudRenderer cloud;Acquisition acquisition;View view;
        bool done=false,frozen=false,save=false,drag=false,pan=false;
        State state,display;std::string notice;
        const auto start=Clock::now();auto fps_time=start;auto next_frame=start;
        std::uint64_t previous_rgb=0,previous_depth=0;double rgb_fps=0,depth_fps=0;unsigned rendered=0;
        double draw_seconds=0,present_seconds=0;
        while (!done) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type==SDL_QUIT) done=true;
                if (event.type==SDL_KEYDOWN && !event.key.repeat) {
                    const auto key=event.key.keysym.sym;
                    if (key==SDLK_ESCAPE) done=true;
                    if (key==SDLK_SPACE) frozen=!frozen;
                    if (key==SDLK_r) view.reset();
                    if (key==SDLK_d) view.full=!view.full;
                    if (key==SDLK_s) save=true;
                }
                if (event.type==SDL_MOUSEBUTTONDOWN) {
                    const int x=event.button.x,y=event.button.y;
                    if (event.button.button==SDL_BUTTON_LEFT) {
                        if (connect_button.contains(x,y)) acquisition.toggle();
                        else if (freeze_button.contains(x,y)) frozen=!frozen;
                        else if (reset_button.contains(x,y)) view.reset();
                        else if (save_button.contains(x,y)) save=true;
                    }
                    if (cloud_view.contains(x,y)) {
                        drag=event.button.button==SDL_BUTTON_LEFT;pan=event.button.button==SDL_BUTTON_RIGHT;
                    }
                }
                if (event.type==SDL_MOUSEBUTTONUP) {drag=false;pan=false;}
                if (event.type==SDL_WINDOWEVENT && event.window.event==SDL_WINDOWEVENT_FOCUS_LOST) {drag=false;pan=false;}
                if (event.type==SDL_MOUSEMOTION) {
                    if (drag) {view.yaw+=event.motion.xrel*.008;view.pitch=std::clamp(view.pitch+event.motion.yrel*.008,-1.5,1.5);}
                    if (pan) {view.pan_x+=event.motion.xrel;view.pan_y+=event.motion.yrel;}
                }
                if (event.type==SDL_MOUSEWHEEL) {
                    int x=0,y=0;SDL_GetMouseState(&x,&y);
                    // Wheel events are useful anywhere in the cloud panel.
                    float lx=0,ly=0;SDL_RenderWindowToLogical(sdl.renderer,x,y,&lx,&ly);
                    if (cloud_card.contains(int(lx),int(ly))) view.distance=std::clamp(view.distance*std::pow(.86,event.wheel.y),.35,12.0);
                }
            }
            state=acquisition.snapshot();if (!frozen) display=state;
            const auto now=Clock::now();const double elapsed=std::chrono::duration<double>(now-fps_time).count();
            if (elapsed>=1) {
                rgb_fps=state.rgb_count>=previous_rgb ? double(state.rgb_count-previous_rgb)/elapsed : 0;
                depth_fps=state.depth_count>=previous_depth ? double(state.depth_count-previous_depth)/elapsed : 0;
                previous_rgb=state.rgb_count;previous_depth=state.depth_count;fps_time=now;
            }
            const auto draw_start=Clock::now();
            draw(cr.get(),state,display,view,cloud,frozen,acquisition.wanted(),rgb_fps,depth_fps,notice);
            cairo_surface_flush(surface.get());
            draw_seconds+=std::chrono::duration<double>(Clock::now()-draw_start).count();
            if (save) {
                try {notice=save_view(surface.get(),display);} catch (const std::exception& e) {notice=e.what();}
                save=false;
            }
            const auto present_start=Clock::now();
            if (SDL_UpdateTexture(sdl.texture,nullptr,cairo_image_surface_get_data(surface.get()),cairo_image_surface_get_stride(surface.get()))<0)
                throw std::runtime_error(SDL_GetError());
            SDL_RenderClear(sdl.renderer);SDL_RenderCopy(sdl.renderer,sdl.texture,nullptr,nullptr);SDL_RenderPresent(sdl.renderer);++rendered;
            present_seconds+=std::chrono::duration<double>(Clock::now()-present_start).count();
            if (run_seconds && std::chrono::duration<double>(now-start).count()>=run_seconds) done=true;
            next_frame+=std::chrono::milliseconds(67);
            if (next_frame<Clock::now()) next_frame=Clock::now();
            std::this_thread::sleep_until(next_frame);
        }
        if (!screenshot.empty() && cairo_surface_write_to_png(surface.get(),screenshot.c_str())!=CAIRO_STATUS_SUCCESS)
            throw std::runtime_error("Cannot write requested screenshot");
        std::ostringstream json;
        const auto average_ms=[](double total,std::uint64_t count){return count ? total*1000/double(count) : 0;};
        json<<"{\n  \"rgb_frames\": "<<state.rgb_count<<", \"depth_frames\": "<<state.depth_count
            <<",\n  \"rgb_last_interval_fps\": "<<rgb_fps<<", \"depth_last_interval_fps\": "<<depth_fps
            <<",\n  \"imu_records\": "<<state.imu_count<<", \"imu_errors\": "<<state.imu_errors
            <<",\n  \"valid_points\": "<<state.valid_points<<", \"rendered_frames\": "<<rendered
            <<",\n  \"timing_ms\": {\"rgb_read\": "<<average_ms(state.rgb_read_seconds,state.rgb_count)
            <<", \"rgb_convert\": "<<average_ms(state.rgb_convert_seconds,state.rgb_count)
            <<", \"depth_read\": "<<average_ms(state.depth_read_seconds,state.depth_count)
            <<", \"depth_geometry_preview\": "<<average_ms(state.depth_convert_seconds,state.depth_count)
            <<", \"ui_draw\": "<<average_ms(draw_seconds,rendered)<<", \"ui_present\": "<<average_ms(present_seconds,rendered)<<"},"
            <<"\n  \"imu_motion_validated\": false,\n  \"last_error\": "<<std::quoted(state.error)<<"\n}\n";
        std::cout<<json.str();
        if (!report.empty()) {std::ofstream file;file.exceptions(std::ios::failbit|std::ios::badbit);file.open(report);file<<json.str();file.close();}
        return run_seconds && (!state.rgb_count || !state.depth_count) ? 1 : 0;
    } catch (const std::exception& e) {std::cerr<<"mars04-viewer: "<<e.what()<<'\n';return 1;}
}
