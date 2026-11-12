#include "camera.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <glob.h>
#include <jpeglib.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

#ifndef JCS_EXTENSIONS
#error "Build with libjpeg-turbo (Debian libjpeg-dev)."
#endif

namespace microcam {
namespace {

constexpr auto capture_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

int xioctl(int fd, unsigned long request, void* arg) {
    int rc;
    do {
        rc = ::ioctl(fd, request, arg);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

std::string system_error(const std::string& path, const char* action, int code) {
    return path + ": " + action + ": " + std::strerror(code);
}

std::vector<std::string> video_devices() {
    glob_t matches{};
    std::vector<std::string> devices;
    if (::glob("/dev/video[0-9]*", GLOB_NOSORT, nullptr, &matches) == 0) {
        for (std::size_t i = 0; i < matches.gl_pathc; ++i) {
            std::string path(matches.gl_pathv[i]);
            if (path.find_first_not_of("0123456789", sizeof("/dev/video") - 1) ==
                std::string::npos) {
                devices.push_back(std::move(path));
            }
        }
    }
    ::globfree(&matches);
    std::sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });
    return devices;
}

std::uint32_t pack_yuv(int y, int red, int green, int blue) {
    const auto r = static_cast<std::uint32_t>(std::clamp((y + red) >> 8, 0, 255));
    const auto g = static_cast<std::uint32_t>(std::clamp((y + green) >> 8, 0, 255));
    const auto b = static_cast<std::uint32_t>(std::clamp((y + blue) >> 8, 0, 255));
    return (r << 16) | (g << 8) | b;
}

} // namespace

struct Camera::JpegDecoder {
    struct Error {
        jpeg_error_mgr manager{};
        std::jmp_buf jump;
    } error;
    jpeg_decompress_struct info{};

    static void fail(j_common_ptr info) {
        auto* error = reinterpret_cast<Error*>(info->err);
        std::longjmp(error->jump, 1);
    }

    static void quiet(j_common_ptr) {}

    bool init() {
        info.err = jpeg_std_error(&error.manager);
        error.manager.error_exit = fail;
        error.manager.output_message = quiet;
        if (setjmp(error.jump)) return false;
        jpeg_create_decompress(&info);
        return true;
    }

    ~JpegDecoder() { jpeg_destroy_decompress(&info); }

    bool decode(const unsigned char* data, std::size_t size, int width, int height,
                std::uint32_t* out) {
        // libjpeg's C error handler jumps here. Keep only trivial locals in
        // this function, and retain the decompressor across frames.
        if (setjmp(error.jump)) {
            jpeg_abort_decompress(&info);
            return false;
        }
        jpeg_mem_src(&info, data, size);
        if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK ||
            info.image_width != static_cast<unsigned>(width) ||
            info.image_height != static_cast<unsigned>(height)) {
            jpeg_abort_decompress(&info);
            return false;
        }

        info.out_color_space = JCS_EXT_BGRX;
        info.dct_method = JDCT_IFAST;
        info.do_fancy_upsampling = FALSE;
        if (!jpeg_start_decompress(&info)) {
            jpeg_abort_decompress(&info);
            return false;
        }
        while (info.output_scanline < info.output_height) {
            JSAMPROW rows[8];
            const auto count = std::min<JDIMENSION>(8, info.output_height - info.output_scanline);
            for (JDIMENSION i = 0; i < count; ++i) {
                rows[i] = reinterpret_cast<JSAMPROW>(
                    out + static_cast<std::size_t>(info.output_scanline + i) * width);
            }
            if (!jpeg_read_scanlines(&info, rows, count)) {
                jpeg_abort_decompress(&info);
                return false;
            }
        }
        if (jpeg_finish_decompress(&info)) return true;
        jpeg_abort_decompress(&info);
        return false;
    }
};

Camera::Camera() = default;

Camera::~Camera() { close(); }

bool Camera::open(const CameraOptions& options, std::string& error) {
    close();
    error.clear();
    if (options.width <= 0 || options.height <= 0 || options.fps <= 0) {
        error = "camera width, height and fps must be positive";
        return false;
    }

    if (!options.device.empty()) {
        if (open_device(options.device, options, false, error)) return true;
        close();
        return false;
    }

    for (const auto& path : video_devices()) {
        std::string reason;
        if (open_device(path, options, true, reason)) {
            error.clear();
            return true;
        }
        close();
        if (error.empty()) error = std::move(reason);
    }
    if (error.empty()) error = "no compatible USB camera found";
    return false;
}

bool Camera::open_device(const std::string& path, const CameraOptions& options,
                         bool usb_only, std::string& error) {
    const auto fail = [&](const char* action) {
        error = system_error(path, action, errno);
        return false;
    };

    fd_ = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) return fail("open");

    v4l2_capability device{};
    if (xioctl(fd_, VIDIOC_QUERYCAP, &device) < 0) return fail("VIDIOC_QUERYCAP");
    const auto caps = (device.capabilities & V4L2_CAP_DEVICE_CAPS)
        ? device.device_caps : device.capabilities;
    if (usb_only && std::strncmp(reinterpret_cast<const char*>(device.bus_info), "usb-", 4) != 0 &&
        std::strncmp(reinterpret_cast<const char*>(device.driver), "uvcvideo", 9) != 0) {
        return false;
    }
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        if (!usb_only) error = path + ": not a streaming video capture node";
        return false;
    }

    v4l2_format negotiated{};
    for (const auto pixel_format : {V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_JPEG, V4L2_PIX_FMT_YUYV}) {
        v4l2_format candidate{};
        candidate.type = capture_type;
        candidate.fmt.pix.width = static_cast<unsigned>(options.width);
        candidate.fmt.pix.height = static_cast<unsigned>(options.height);
        candidate.fmt.pix.pixelformat = pixel_format;
        candidate.fmt.pix.field = V4L2_FIELD_ANY;
        if (device.capabilities & V4L2_CAP_EXT_PIX_FORMAT) {
            candidate.fmt.pix.priv = V4L2_PIX_FMT_PRIV_MAGIC;
        }
        if (xioctl(fd_, VIDIOC_S_FMT, &candidate) == 0 &&
            candidate.fmt.pix.pixelformat == pixel_format) {
            negotiated = candidate;
            break;
        }
    }

    const auto& pix = negotiated.fmt.pix;
    if (!pix.pixelformat) {
        error = path + ": camera could not negotiate MJPEG, JPEG or YUYV";
        return false;
    }
    if (!pix.width || !pix.height || pix.width > INT_MAX || pix.height > INT_MAX ||
        static_cast<std::uint64_t>(pix.width) * pix.height > decoded_.max_size()) {
        error = path + ": camera returned invalid frame dimensions";
        return false;
    }
    width_ = static_cast<int>(pix.width);
    height_ = static_cast<int>(pix.height);
    pixel_format_ = pix.pixelformat;

    if (pixel_format_ == V4L2_PIX_FMT_YUYV) {
        stride_ = std::max<std::size_t>(pix.bytesperline, static_cast<std::size_t>(width_) * 2);
        if ((width_ & 1) ||
            static_cast<std::uint64_t>(stride_) * height_ > SIZE_MAX) {
            error = path + ": camera returned invalid YUYV dimensions or row stride";
            return false;
        }

        auto encoding = pix.priv == V4L2_PIX_FMT_PRIV_MAGIC ? pix.ycbcr_enc : 0;
        auto quant = pix.priv == V4L2_PIX_FMT_PRIV_MAGIC ? pix.quantization : 0;
        if (encoding == V4L2_YCBCR_ENC_DEFAULT) {
            encoding = V4L2_MAP_YCBCR_ENC_DEFAULT(pix.colorspace);
        }
        if (quant == V4L2_QUANTIZATION_DEFAULT) {
            quant = V4L2_MAP_QUANTIZATION_DEFAULT(false, pix.colorspace, encoding);
        }
        const bool full = quant == V4L2_QUANTIZATION_FULL_RANGE;

        double kr = 0.299;
        double kb = 0.114;
        if (encoding == V4L2_YCBCR_ENC_709 || encoding == V4L2_YCBCR_ENC_XV709) {
            kr = 0.2126;
            kb = 0.0722;
        } else if (encoding == V4L2_YCBCR_ENC_BT2020) {
            kr = 0.2627;
            kb = 0.0593;
        } else if (encoding == V4L2_YCBCR_ENC_SMPTE240M) {
            kr = 0.2122;
            kb = 0.0865;
        }
        const double kg = 1.0 - kr - kb;
        const double chroma_scale = full ? 256.0 : 256.0 * 255.0 / 224.0;
        const auto fixed = [chroma_scale](double k) {
            return static_cast<int>(k * chroma_scale + 0.5);
        };
        yuv_ = {full ? 0 : 16, full ? 256 : 298, fixed(2.0 * (1.0 - kr)),
                fixed(2.0 * kb * (1.0 - kb) / kg), fixed(2.0 * kr * (1.0 - kr) / kg),
                fixed(2.0 * (1.0 - kb))};
    } else {
        jpeg_ = std::make_unique<JpegDecoder>();
        if (!jpeg_->init()) {
            error = path + ": could not initialize the MJPEG decoder";
            return false;
        }
    }

    v4l2_streamparm params{};
    params.type = capture_type;
    if (xioctl(fd_, VIDIOC_G_PARM, &params) == 0 &&
        (params.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        params.parm.capture.timeperframe.numerator = 1;
        params.parm.capture.timeperframe.denominator = static_cast<unsigned>(options.fps);
        xioctl(fd_, VIDIOC_S_PARM, &params); // The driver may limit the rate.
    }

    if (options.fixed_exposure_fps) {
        v4l2_control control{};
        control.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
        control.value = 0;
        if (xioctl(fd_, VIDIOC_S_CTRL, &control) == 0) {
            std::fprintf(stderr, "[camera] %s: dynamic exposure frame rate disabled\n", path.c_str());
        } else {
            std::fprintf(stderr, "[camera] %s: could not disable dynamic exposure frame rate: %s\n",
                         path.c_str(), std::strerror(errno));
        }
    }

    v4l2_requestbuffers requested{};
    requested.count = 3;
    requested.type = capture_type;
    requested.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_, VIDIOC_REQBUFS, &requested) < 0) return fail("VIDIOC_REQBUFS");
    if (requested.count < 2) {
        error = path + ": camera needs at least two MMAP buffers";
        return false;
    }

    buffers_.reserve(requested.count);
    for (unsigned index = 0; index < requested.count; ++index) {
        v4l2_buffer buffer{};
        buffer.index = index;
        buffer.type = capture_type;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) return fail("VIDIOC_QUERYBUF");
        void* data = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                            fd_, buffer.m.offset);
        if (data == MAP_FAILED) return fail("mmap");
        buffers_.push_back({data, buffer.length});
        if (!queue(index)) return fail("VIDIOC_QBUF");
    }

    auto type = capture_type;
    if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) return fail("VIDIOC_STREAMON");
    streaming_ = true;
    path_ = path;
    const auto* name = reinterpret_cast<const char*>(device.card);
    name_.assign(name, ::strnlen(name, sizeof(device.card)));
    return true;
}

void Camera::close() noexcept {
    if (streaming_) {
        auto type = capture_type;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
        streaming_ = false;
    }
    for (const auto& buffer : buffers_) ::munmap(buffer.data, buffer.size);
    buffers_.clear();
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    jpeg_.reset();
    path_.clear();
    name_.clear();
    width_ = 0;
    height_ = 0;
    pixel_format_ = 0;
    stride_ = 0;
}

const char* Camera::format() const {
    switch (pixel_format_) {
    case V4L2_PIX_FMT_MJPEG: return "MJPEG";
    case V4L2_PIX_FMT_JPEG: return "JPEG";
    case V4L2_PIX_FMT_YUYV: return "YUYV";
    default: return "";
    }
}

bool Camera::queue(unsigned index) {
    v4l2_buffer buffer{};
    buffer.index = index;
    buffer.type = capture_type;
    buffer.memory = V4L2_MEMORY_MMAP;
    return xioctl(fd_, VIDIOC_QBUF, &buffer) == 0;
}

Camera::Read Camera::latest(Frame& frame, bool decode, std::string& error) {
    error.clear();
    if (!streaming_) return Read::disconnected;
    const auto fail = [&](const char* action) {
        error = system_error(path_, action, errno);
        return Read::disconnected;
    };

    v4l2_buffer newest{};
    bool have_frame = false;
    // Bound the drain so a fast producer cannot starve touch or display work.
    for (std::size_t i = 0; i < buffers_.size(); ++i) {
        v4l2_buffer current{};
        current.type = capture_type;
        current.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_DQBUF, &current) < 0) {
            if (errno == EAGAIN) break;
            return fail("VIDIOC_DQBUF");
        }
        if (current.index >= buffers_.size()) {
            error = path_ + ": camera returned an invalid capture buffer index";
            return Read::disconnected;
        }
        const bool usable = decode && !(current.flags & V4L2_BUF_FLAG_ERROR) &&
            current.bytesused && current.bytesused <= buffers_[current.index].size;
        if (usable) {
            if (have_frame && !queue(newest.index)) return fail("VIDIOC_QBUF");
            newest = current;
            have_frame = true;
        } else if (!queue(current.index)) {
            return fail("VIDIOC_QBUF");
        }
    }
    if (!have_frame) return Read::no_frame;

    const auto* data = static_cast<const unsigned char*>(buffers_[newest.index].data);
    const bool decoded = decode_frame(data, newest.bytesused);
    if (!queue(newest.index)) return fail("VIDIOC_QBUF");
    if (!decoded) return Read::no_frame;
    frame.width = width_;
    frame.height = height_;
    frame.pixels.swap(decoded_);
    return Read::frame;
}

bool Camera::decode_frame(const unsigned char* data, std::size_t size) {
    decoded_.resize(static_cast<std::size_t>(width_) * height_);
    if (jpeg_) return jpeg_->decode(data, size, width_, height_, decoded_.data());
    return decode_yuyv(data, size);
}

bool Camera::decode_yuyv(const unsigned char* data, std::size_t size) {
    const auto width = static_cast<std::size_t>(width_);
    if (size < stride_ * (height_ - 1) + 2 * width) return false;

    auto* out = decoded_.data();
    for (int row = 0; row < height_; ++row) {
        const auto* in = data + stride_ * row;
        for (std::size_t x = 0; x < width; x += 2, in += 4) {
            const int u = in[1] - 128;
            const int v = in[3] - 128;
            const int red = yuv_.red_v * v;
            const int green = -yuv_.green_u * u - yuv_.green_v * v;
            const int blue = yuv_.blue_u * u;
            const int y0 = yuv_.y_scale * (in[0] - yuv_.y_offset) + 128;
            const int y1 = yuv_.y_scale * (in[2] - yuv_.y_offset) + 128;
            *out++ = pack_yuv(y0, red, green, blue);
            *out++ = pack_yuv(y1, red, green, blue);
        }
    }
    return true;
}

} // namespace microcam
