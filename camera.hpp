#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace microcam {

struct Frame {
    int width = 0;
    int height = 0;
    // Tightly packed B, G, R, X bytes on little-endian systems.
    std::vector<std::uint32_t> pixels;
};

struct CameraOptions {
    std::string device;
    int width = 640;
    int height = 480;
    int fps = 30;
    bool fixed_exposure_fps = false;
};

class Camera {
public:
    enum class Read { no_frame, frame, disconnected };

    Camera();
    ~Camera();
    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    bool open(const CameraOptions& options, std::string& error);
    void close() noexcept;
    int fd() const { return fd_; }
    const std::string& path() const { return path_; }
    const std::string& name() const { return name_; }
    int width() const { return width_; }
    int height() const { return height_; }
    const char* format() const;

    // Drain queued buffers without blocking. On success, return the newest
    // decoded frame. When decode is false, requeue buffers without decoding.
    Read latest(Frame& frame, bool decode, std::string& error);

private:
    struct Mapping {
        void* data;
        std::size_t size;
    };

    struct YuvConversion {
        int y_offset = 16;
        int y_scale = 298;
        int red_v = 409;
        int green_u = 100;
        int green_v = 208;
        int blue_u = 516;
    };

    struct JpegDecoder;

    bool open_device(const std::string& path, const CameraOptions& options,
                     bool usb_only, std::string& error);
    bool queue(unsigned index);
    bool decode_frame(const unsigned char* data, std::size_t size);
    bool decode_yuyv(const unsigned char* data, std::size_t size);

    int fd_ = -1;
    bool streaming_ = false;
    std::string path_;
    std::string name_;
    int width_ = 0;
    int height_ = 0;
    std::uint32_t pixel_format_ = 0;
    std::size_t stride_ = 0;
    YuvConversion yuv_;
    std::vector<Mapping> buffers_;
    std::unique_ptr<JpegDecoder> jpeg_;
    std::vector<std::uint32_t> decoded_;
};

} // namespace microcam
