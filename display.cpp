#include "display.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fcntl.h>
#include <initializer_list>
#include <limits>
#include <linux/fb.h>
#include <png.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace microcam {
namespace {

constexpr int glyph_width = 5;
constexpr int glyph_height = 7;
constexpr int font_scale = 2;
constexpr int advance = (glyph_width + 1) * font_scale;
constexpr int padding = 2;
constexpr int text_height = glyph_height * font_scale + 2 * padding;
constexpr int status_margin = 4;
constexpr int status_height = 2 * text_height;
constexpr int button_size = 60;
constexpr int button_margin = 12;
constexpr int button_touch_margin = 56;
constexpr int flash_border = 6;

using Glyph = std::array<std::uint8_t, glyph_height>;

// One five-bit mask per row, with the leftmost pixel in bit four.
constexpr Glyph digits[] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},    {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30},    {2,6,10,18,31,2,2},   {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},     {14,17,17,14,17,17,14},
    {14,17,17,15,1,1,14},
};

constexpr Glyph letters[] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14}, // A-C
    {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16}, // D-F
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},      // G-I
    {7,2,2,2,2,18,12},    {17,18,20,24,20,18,17},{16,16,16,16,16,16,31}, // J-L
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14}, // M-O
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17}, // P-R
    {15,16,16,14,1,1,30},  {31,4,4,4,4,4,4},     {17,17,17,17,17,17,14}, // S-U
    {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10},{17,17,10,4,10,17,17}, // V-X
    {17,17,10,4,4,4,4},    {31,1,2,4,8,16,31},                            // Y-Z
};

constexpr Glyph glyph(char ch) {
    if (ch >= '0' && ch <= '9') return digits[ch - '0'];
    if (ch >= 'a' && ch <= 'z') return letters[ch - 'a'];
    if (ch >= 'A' && ch <= 'Z') return letters[ch - 'A'];
    switch (ch) {
    case ' ': return {};
    case '.': return {0,0,0,0,0,6,6};
    case ':': return {0,6,6,0,6,6,0};
    case '-': return {0,0,0,31,0,0,0};
    case '+': return {0,4,4,31,4,4,0};
    case '_': return {0,0,0,0,0,0,31};
    case '/': return {1,1,2,4,8,16,16};
    case '(': return {2,4,8,8,8,4,2};
    case ')': return {8,4,2,2,2,4,8};
    default:  return {14,17,1,2,4,0,4};
    }
}

std::string_view clip_text(std::string_view text, int width) {
    return text.substr(0, static_cast<std::size_t>(std::max(width - 2 * padding, 0) /
                                                  advance));
}

int tile_width(std::string_view clipped) {
    return static_cast<int>(clipped.size()) * advance + 2 * padding;
}

template <typename Plot>
void text_pixels(std::string_view text, Plot plot) {
    int x = padding;
    for (const char ch : text) {
        const auto bitmap = glyph(ch);
        for (int gy = 0; gy < glyph_height; ++gy) {
            for (int gx = 0; gx < glyph_width; ++gx) {
                if ((bitmap[gy] & (1 << (glyph_width - gx - 1))) == 0) continue;
                for (int py = 0; py < font_scale; ++py) {
                    for (int px = 0; px < font_scale; ++px) {
                        plot(x + gx * font_scale + px, padding + gy * font_scale + py);
                    }
                }
            }
        }
        x += advance;
    }
}

std::uint32_t field_max(const fb_bitfield& field) {
    return static_cast<std::uint32_t>((std::uint64_t{1} << field.length) - 1);
}

bool valid_field(const fb_bitfield& field, unsigned bits) {
    return field.length == 0 ||
           (field.msb_right == 0 && field.offset < bits && field.length <= bits - field.offset);
}

std::uint32_t field_mask(const fb_bitfield& field) {
    return field.length == 0 ? 0 : field_max(field) << field.offset;
}

void make_channel(std::array<std::uint32_t, 256>& table, const fb_bitfield& field) {
    const std::uint64_t maximum = field_max(field);
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = static_cast<std::uint32_t>((i * maximum + 127) / 255) << field.offset;
    }
}

void write_pixel(std::uint8_t* dst, int bytes, std::uint32_t color) {
    // memcpy also works when a pitch or pixel offset is not word aligned.
    if (bytes == 2) {
        const auto packed = static_cast<std::uint16_t>(color);
        std::memcpy(dst, &packed, sizeof(packed));
    } else {
        std::memcpy(dst, &color, sizeof(color));
    }
}


struct IconPoint { double x, y; };

double segment_distance_sq(IconPoint p, IconPoint a, IconPoint b) {
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) /
                                (dx * dx + dy * dy), 0.0, 1.0);
    const double x = p.x - a.x - t * dx, y = p.y - a.y - t * dy;
    return x * x + y * y;
}

bool stroke(IconPoint p, std::initializer_list<IconPoint> points, double radius = 1.45) {
    for (auto it = points.begin() + 1; it != points.end(); ++it) {
        if (segment_distance_sq(p, *(it - 1), *it) <= radius * radius) return true;
    }
    return false;
}

double rounded_rect_distance(IconPoint p, double x, double y, double w, double h, double r) {
    const double dx = std::abs(p.x - x) - w / 2 + r;
    const double dy = std::abs(p.y - y) - h / 2 + r;
    const double ox = std::max(0.0, dx), oy = std::max(0.0, dy);
    return std::sqrt(ox * ox + oy * oy) + std::min(0.0, std::max(dx, dy)) - r;
}

enum class ButtonIcon { reset, camera, locked, unlocked, saved, failed, settings, power };

bool icon_pixel(ButtonIcon icon, IconPoint p) {
    switch (icon) {
    case ButtonIcon::reset: {
        const double dx = p.x - 30, dy = p.y - 30;
        const double r = dx * dx + dy * dy;
        const bool arc = r >= 16.5 * 16.5 && r <= 19.5 * 19.5 &&
                         !(dx > 0 && dy < 0 && dy > -0.65 * dx);
        return arc || stroke(p, {{45,16}, {45,22}, {39,22}}) ||
               stroke(p, {{22,28}, {25,25}, {25,35}}, 1.25) ||
               stroke(p, {{31,28}, {38,35}}, 1.2) ||
               stroke(p, {{38,28}, {31,35}}, 1.2);
    }
    case ButtonIcon::camera: {
        const double dx = p.x - 30, dy = p.y - 32;
        const double r = dx * dx + dy * dy;
        return (r >= 5 * 5 && r <= 8 * 8) ||
            stroke(p, {{16,23}, {21,23}, {24,18}, {36,18}, {39,23}, {44,23}, {47,26},
                       {47,41}, {44,44}, {16,44}, {13,41}, {13,26}, {16,23}});
    }
    case ButtonIcon::locked:
    case ButtonIcon::unlocked: {
        const bool locked = icon == ButtonIcon::locked;
        const double cy = locked ? 23 : 21;
        const double dx = p.x - 30, dy = p.y - cy;
        const double r = dx * dx + dy * dy;
        return std::abs(rounded_rect_distance(p, 30, 36, 26, 19, 3.5)) <= 1.45 ||
               (dy <= 0 && r >= 6.5 * 6.5 && r <= 9.5 * 9.5) ||
               stroke(p, {{22,cy}, {22,26.5}}) ||
               stroke(p, {{38,cy}, {38,26.5 + (locked ? 0.0 : -3.5)}});
    }
    case ButtonIcon::saved:
        return stroke(p, {{19,31}, {27,39}, {42,23}}, 1.8);
    case ButtonIcon::power: {
        const double dx = p.x - 30, dy = p.y - 31;
        const double r = dx * dx + dy * dy;
        return (r >= 12 * 12 && r <= 15 * 15 && (dy >= 0 || std::abs(dx) >= 7)) ||
               stroke(p, {{30,14}, {30,29}});
    }
    case ButtonIcon::settings: {
        const double dx = p.x - 30, dy = p.y - 30;
        const double radius = std::sqrt(dx * dx + dy * dy);
        const double tooth = std::cos(8 * std::atan2(dy, dx));
        return std::abs(radius - 5.5) <= 1.35 ||
               std::abs(radius - (13 + 2.6 * std::max(0.0, tooth))) <= 1.45;
    }
    case ButtonIcon::failed:
        return stroke(p, {{22,22}, {38,38}}, 1.7) || stroke(p, {{22,38}, {38,22}}, 1.7);
    }
    return false;
}

struct ButtonRow { int start, width; };

const std::array<ButtonRow, button_size>& button_rows() {
    static const auto rows = [] {
        std::array<ButtonRow, button_size> result{};
        for (int y = 0; y < button_size; ++y) {
            int x = 0;
            while (x < button_size / 2 &&
                   rounded_rect_distance({x + 0.5, y + 0.5}, 30, 30, 60, 60, 13) > 0) ++x;
            result[y] = {x, button_size - 2 * x};
        }
        return result;
    }();
    return rows;
}

template <typename Pack>
void render_button(std::vector<std::uint8_t>& pixels, int pixel_bytes, ButtonIcon icon, Pack pack) {
    const unsigned accent = icon == ButtonIcon::locked ? 0xffb62f :
                            icon == ButtonIcon::saved ? 0x75efa8 :
                            icon == ButtonIcon::failed ? 0xff8383 : 0xf4f7fa;
    const unsigned border = icon == ButtonIcon::locked || icon == ButtonIcon::saved ||
                            icon == ButtonIcon::failed ? accent : 0x86939d;
    const unsigned background = icon == ButtonIcon::saved ? 0x16372c :
                                icon == ButtonIcon::failed ? 0x3f2027 : 0x22272c;
    pixels.resize(button_size * button_size * pixel_bytes);
    constexpr int samples = 4;
    for (int y = 0; y < button_size; ++y) {
        const auto span = button_rows()[y];
        for (int x = span.start; x < span.start + span.width; ++x) {
            unsigned red = 0, green = 0, blue = 0;
            for (int sy = 0; sy < samples; ++sy) {
                for (int sx = 0; sx < samples; ++sx) {
                    const IconPoint p{x + (sx + 0.5) / samples, y + (sy + 0.5) / samples};
                    const auto edge = rounded_rect_distance(p, 30, 30, 60, 60, 13);
                    const auto color = icon_pixel(icon, p) ? accent : edge > -1.4 ? border : background;
                    red += (color >> 16) & 255;
                    green += (color >> 8) & 255;
                    blue += color & 255;
                }
            }
            constexpr unsigned count = samples * samples;
            const auto color = (red / count << 16) | (green / count << 8) | (blue / count);
            write_pixel(pixels.data() + (y * button_size + x) * pixel_bytes, pixel_bytes, pack(color));
        }
    }
}

void copy_button_row(std::uint8_t* row, const std::vector<std::uint8_t>& pixels,
                     Rect button, int y, int pixel_bytes) {
    const int local_y = y - button.y;
    const auto span = button_rows()[local_y];
    const auto byte_offset = static_cast<std::size_t>(local_y * button_size + span.start) * pixel_bytes;
    std::memcpy(row + (button.x + span.start) * pixel_bytes, pixels.data() + byte_offset,
                static_cast<std::size_t>(span.width) * pixel_bytes);
}

void nearest_axis(std::vector<int>& lookup, int source, int dest) {
    lookup.resize(static_cast<std::size_t>(dest));
    for (int i = 0; i < dest; ++i) {
        lookup[i] = static_cast<int>(static_cast<std::uint64_t>(i) * source / dest);
    }
}

void checked_ioctl(int fd, unsigned long request, void* value, const std::string& path,
                   const char* operation) {
    int result;
    do {
        result = ::ioctl(fd, request, value);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(), path + " " + operation);
    }
}

}  // namespace

Framebuffer::Framebuffer(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "open " + path);
    }

    try {
        fb_fix_screeninfo fixed{};
        fb_var_screeninfo var{};
        checked_ioctl(fd_, FBIOGET_FSCREENINFO, &fixed, path, "FBIOGET_FSCREENINFO");
        checked_ioctl(fd_, FBIOGET_VSCREENINFO, &var, path, "FBIOGET_VSCREENINFO");

        const unsigned bits = var.bits_per_pixel;
        if ((bits != 16 && bits != 32) || fixed.type != FB_TYPE_PACKED_PIXELS ||
            fixed.visual != FB_VISUAL_TRUECOLOR || var.grayscale != 0 || var.nonstd != 0 ||
            !valid_field(var.red, bits) || !valid_field(var.green, bits) ||
            !valid_field(var.blue, bits) || !valid_field(var.transp, bits)) {
            throw std::runtime_error(path + " needs a packed 16- or 32-bit true-color mode");
        }
        const auto r = field_mask(var.red);
        const auto g = field_mask(var.green);
        const auto b = field_mask(var.blue);
        opaque_ = field_mask(var.transp);
        if (!r || !g || !b || (r & g) || (r & b) || (g & b) || ((r | g | b) & opaque_)) {
            throw std::runtime_error(path + " reported invalid RGB bit fields");
        }

        pixel_bytes_ = static_cast<int>(bits / 8);
        const auto x_end = static_cast<std::uint64_t>(var.xoffset) + var.xres;
        const auto y_end = static_cast<std::uint64_t>(var.yoffset) + var.yres;
        const auto end_of_row = x_end * pixel_bytes_;
        constexpr auto max_dimension = static_cast<unsigned>(std::numeric_limits<int>::max());
        if (var.xres == 0 || var.yres == 0 ||
            var.xres > max_dimension || var.yres > max_dimension ||
            x_end > var.xres_virtual || y_end > var.yres_virtual ||
            end_of_row > fixed.line_length ||
            (y_end - 1) * fixed.line_length + end_of_row > fixed.smem_len) {
            throw std::runtime_error(path + " reported invalid framebuffer dimensions");
        }

        native32_ = bits == 32 && r == 0xff0000 && g == 0x00ff00 && b == 0x0000ff && !opaque_;
        rgb565_ = bits == 16 && r == 0xf800 && g == 0x07e0 && b == 0x001f;
        make_channel(red_, var.red);
        make_channel(green_, var.green);
        make_channel(blue_, var.blue);
        rgb_ = {{{var.red.offset, field_max(var.red)},
                 {var.green.offset, field_max(var.green)},
                 {var.blue.offset, field_max(var.blue)}}};
        width_ = static_cast<int>(var.xres);
        height_ = static_cast<int>(var.yres);
        pitch_ = fixed.line_length;
        row_.resize(static_cast<std::size_t>(width_) * pixel_bytes_);
        composite_row_.resize(row_.size());
        black_row_.resize(row_.size());
        if (opaque_) {
            for (std::size_t i = 0; i < black_row_.size(); i += pixel_bytes_) {
                write_pixel(black_row_.data() + i, pixel_bytes_, opaque_);
            }
        }
        status_rows_.resize(status_height);
        mapping_size_ = fixed.smem_len;
        auto* addr = ::mmap(nullptr, mapping_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (addr == MAP_FAILED) {
            throw std::system_error(errno, std::generic_category(), "mmap " + path);
        }
        mapping_ = static_cast<std::uint8_t*>(addr);
        visible_ = mapping_ + static_cast<std::size_t>(var.yoffset) * pitch_ +
                   static_cast<std::size_t>(var.xoffset) * pixel_bytes_;
    } catch (...) {
        ::close(fd_);
        throw;
    }
}

Framebuffer::~Framebuffer() {
    ::munmap(mapping_, mapping_size_);
    ::close(fd_);
}

std::uint8_t* Framebuffer::at(int x, int y) const noexcept {
    return visible_ + static_cast<std::size_t>(y) * pitch_ +
           static_cast<std::size_t>(x) * pixel_bytes_;
}

std::uint32_t Framebuffer::pack(std::uint32_t bgrx) const noexcept {
    return opaque_ | red_[(bgrx >> 16) & 255] | green_[(bgrx >> 8) & 255] | blue_[bgrx & 255];
}

void Framebuffer::clear() {
    for (int y = 0; y < height_; ++y) {
        std::memcpy(at(0, y), black_row_.data(), black_row_.size());
    }
    dest_width_ = 0;
    dest_height_ = 0;
    flash_visible_ = false;
    prepare_status({});
}

void Framebuffer::convert_row(const std::uint32_t* input, int width) {
    if (rgb565_) {
        // Truncate BGRX to 5/6/5 bits. This hot path needs no lookup tables.
        auto* output = row_.data();
        for (int x = 0; x < width; ++x, output += 2) {
            const auto p = input[columns_[x]];
            const auto color = static_cast<std::uint16_t>(
                ((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
            std::memcpy(output, &color, sizeof(color));
        }
    } else if (native32_) {
        auto* output = row_.data();
        for (int x = 0; x < width; ++x, output += 4) {
            const auto color = input[columns_[x]];
            std::memcpy(output, &color, sizeof(color));
        }
    } else {
        auto* output = row_.data();
        for (int x = 0; x < width; ++x, output += pixel_bytes_) {
            write_pixel(output, pixel_bytes_, pack(input[columns_[x]]));
        }
    }
}

void Framebuffer::draw(const std::uint32_t* pixels, int frame_width, int frame_height,
                       int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh,
                       std::string_view status, CaptureFeedback feedback, bool capture_flash,
                       bool zoom_locked, bool power_enabled) {
    if (!pixels || frame_width <= 0 || frame_height <= 0 ||
        sx < 0 || sy < 0 || sw <= 0 || sh <= 0 || sx > frame_width - sw || sy > frame_height - sh ||
        dx < 0 || dy < 0 || dw <= 0 || dh <= 0 || dx > width_ - dw || dy > height_ - dh) {
        throw std::invalid_argument("invalid framebuffer draw rectangle");
    }

    if (dx != dest_x_ || dy != dest_y_ || dw != dest_width_ || dh != dest_height_) {
        // The letterbox bars only need repainting when the destination moves.
        clear();
        dest_x_ = dx;
        dest_y_ = dy;
        dest_width_ = dw;
        dest_height_ = dh;
    }
    if (sw != source_width_ || columns_.size() != static_cast<std::size_t>(dw)) {
        nearest_axis(columns_, sw, dw);
        source_width_ = sw;
    }
    if (sh != source_height_ || rows_.size() != static_cast<std::size_t>(dh)) {
        nearest_axis(rows_, sh, dh);
        source_height_ = sh;
    }

    const bool had_status = !status_text_.empty();
    prepare_status(status);
    const auto button = screenshot_button();
    if (button.w) prepare_button(feedback);
    const auto reset = reset_button();
    if (reset.w) prepare_reset_button();
    const auto lock = lock_button();
    if (lock.w) prepare_lock_button(zoom_locked);
    const auto gear = settings_button(power_enabled);
    if (gear.w && settings_pixels_.empty()) {
        render_button(settings_pixels_, pixel_bytes_, ButtonIcon::settings,
                      [this](auto color) { return pack(color); });
    }
    const auto power = power_enabled ? power_button() : Rect{};
    if (power.w && power_pixels_.empty()) {
        render_button(power_pixels_, pixel_bytes_, ButtonIcon::power,
                      [this](auto color) { return pack(color); });
    }
    const bool paint_status = had_status || !status_text_.empty();
    const bool repaint_border = flash_visible_ || capture_flash;
    flash_visible_ = capture_flash;
    const auto flash_color = capture_flash ? pack(0xffffff) : 0;
    const int first_y = repaint_border ? 0 :
        std::min({dy, paint_status ? status_margin : dy, button.w ? button.y : dy,
                  lock.w ? lock.y : dy, gear.w ? gear.y : dy, power.w ? power.y : dy});
    const int last_y = repaint_border ? height_ :
        std::max({dy + dh, paint_status ? std::min(height_, status_margin + status_height) : 0,
                  button.y + button.h, lock.y + lock.h, gear.y + gear.h, power.y + power.h});
    const auto row_bytes = static_cast<std::size_t>(dw) * pixel_bytes_;
    const auto dest_offset = static_cast<std::size_t>(dx) * pixel_bytes_;
    const std::uint8_t* output = nullptr;
    int previous_y = -1;
    for (int y = first_y; y < last_y; ++y) {
        const bool has_video = y >= dy && y < dy + dh;
        if (has_video) {
            const int source_y = rows_[y - dy];
            if (source_y != previous_y) {
                const auto* input = pixels + static_cast<std::size_t>(sy + source_y) * frame_width + sx;
                if (native32_ && sw == dw) {
                    output = reinterpret_cast<const std::uint8_t*>(input);
                } else {
                    convert_row(input, dw);
                    output = row_.data();
                }
                previous_y = source_y;
            }
        }
        const bool status_row = paint_status && y >= status_margin && y < status_margin + status_height;
        const bool button_row = button.w && y >= button.y && y < button.y + button.h;
        const bool lock_row = lock.w && y >= lock.y && y < lock.y + lock.h;
        const bool gear_row = gear.w && y >= gear.y && y < gear.y + gear.h;
        const bool power_row = power.w && y >= power.y && y < power.y + power.h;
        if (status_row || button_row || lock_row || gear_row || power_row || repaint_border) {
            // Write the overlay and its underlying video together. Include the bars
            // so a shorter or hidden label clears any pixels left by the last one.
            auto* row = composite_row_.data();
            if (!has_video || dw != width_) std::memcpy(row, black_row_.data(), black_row_.size());
            if (has_video) std::memcpy(row + dest_offset, output, row_bytes);
            if (status_row) {
                for (const auto& pixel : status_rows_[y - status_margin]) {
                    write_pixel(row + pixel.offset, pixel_bytes_, pixel.color);
                }
            }
            if (button_row) {
                copy_button_row(row, button_pixels_, button, y, pixel_bytes_);
                if (reset.w) copy_button_row(row, reset_pixels_, reset, y, pixel_bytes_);
            }
            if (lock_row) copy_button_row(row, lock_pixels_, lock, y, pixel_bytes_);
            if (gear_row) copy_button_row(row, settings_pixels_, gear, y, pixel_bytes_);
            if (power_row) copy_button_row(row, power_pixels_, power, y, pixel_bytes_);
            if (capture_flash) {
                if (y < flash_border || y >= height_ - flash_border) {
                    for (int x = 0; x < width_; ++x) {
                        write_pixel(row + x * pixel_bytes_, pixel_bytes_, flash_color);
                    }
                } else {
                    for (int x = 0; x < std::min(width_, flash_border); ++x) {
                        write_pixel(row + x * pixel_bytes_, pixel_bytes_, flash_color);
                        write_pixel(row + (width_ - 1 - x) * pixel_bytes_, pixel_bytes_, flash_color);
                    }
                }
            }
            std::memcpy(at(0, y), row, composite_row_.size());
        } else if (has_video) {
            // Reuse repeated source scanlines from RAM without reading the framebuffer.
            std::memcpy(at(dx, y), output, row_bytes);
        }
    }
}

void Framebuffer::prepare_status(std::string_view text) {
    if (text == status_text_) return;
    status_text_ = text;
    for (auto& row : status_rows_) row.clear();
    const auto split = text.find('\n');
    const std::string_view lines[] = {text.substr(0, split),
        split == std::string_view::npos ? std::string_view{} : text.substr(split + 1)};
    for (int line = 0; line < 2; ++line) {
        const auto clipped = clip_text(lines[line], width_ - status_margin);
        if (clipped.empty() || height_ < status_margin + (line + 1) * text_height) continue;

        constexpr std::uint8_t outline = 1, fill = 2;
        const auto stride = static_cast<std::size_t>(tile_width(clipped));
        std::vector<std::uint8_t> mask(stride * text_height);
        text_pixels(clipped, [&](int x, int y) {
            for (int yy = y - 1; yy <= y + 1; ++yy) {
                for (int xx = x - 1; xx <= x + 1; ++xx) {
                    auto& pixel = mask[static_cast<std::size_t>(yy) * stride + xx];
                    if (!pixel) pixel = outline;
                }
            }
            mask[static_cast<std::size_t>(y) * stride + x] = fill;
        });
        const auto yellow = pack(0x00ffff00);
        for (int y = 0; y < text_height; ++y) {
            auto& row = status_rows_[line * text_height + y];
            for (std::size_t x = 0; x < stride; ++x) {
                const auto pixel = mask[static_cast<std::size_t>(y) * stride + x];
                if (pixel) row.push_back({(status_margin + x) * pixel_bytes_,
                                          pixel == fill ? yellow : opaque_});
            }
        }
    }
}

Rect Framebuffer::screenshot_button() const noexcept {
    if (width_ < button_size + button_margin || height_ < button_size + button_margin) return {};
    return {width_ - button_size - button_margin, height_ - button_size - button_margin,
            button_size, button_size};
}

Rect Framebuffer::screenshot_touch_area() const noexcept {
    const auto button = screenshot_button();
    if (!button.w) return {};
    const int x = std::max(0, button.x - button_touch_margin);
    const int y = std::max(0, button.y - button_touch_margin);
    return {x, y, width_ - x, height_ - y};
}

Rect Framebuffer::reset_button() const noexcept {
    const auto right = screenshot_button();
    if (!right.w || width_ < 2 * (button_size + button_margin + button_touch_margin)) return {};
    return {button_margin, right.y, right.w, right.h};
}

Rect Framebuffer::reset_touch_area() const noexcept {
    const auto button = reset_button();
    if (!button.w) return {};
    const int y = std::max(0, button.y - button_touch_margin);
    return {0, y, button.x + button.w + button_touch_margin, height_ - y};
}

Rect Framebuffer::lock_button() const noexcept {
    if (width_ < button_size + button_margin || height_ < button_size + button_margin) return {};
    return {width_ - button_size - button_margin, button_margin, button_size, button_size};
}

Rect Framebuffer::lock_touch_area() const noexcept {
    const auto button = lock_button();
    if (!button.w) return {};
    const int x = std::max(0, button.x - button_touch_margin);
    return {x, 0, width_ - x, std::min(height_, button.y + button.h + button_touch_margin)};
}

Rect Framebuffer::settings_button(bool power_enabled) const noexcept {
    if (width_ < 256 || height_ < 300) return {};
    const int offset = power_enabled && power_button().w ? button_size + 24 : 0;
    return {button_margin, status_margin + status_height + button_margin + offset,
            button_size, button_size};
}

Rect Framebuffer::settings_touch_area(bool power_enabled) const noexcept {
    const auto gear = settings_button(power_enabled);
    if (!gear.w) return {};
    return {0, gear.y - 10, gear.x + gear.w + 18, gear.h + 30};
}

Rect Framebuffer::power_button() const noexcept {
    if (width_ < 320 || height_ < 390) return {};
    return {button_margin, status_margin + status_height + button_margin,
            button_size, button_size};
}

Rect Framebuffer::power_touch_area() const noexcept {
    const auto power = power_button();
    if (!power.w) return {};
    return {0, power.y - 10, power.x + power.w + 18, power.h + 20};
}

PowerLayout Framebuffer::power_layout() const noexcept {
    const int w = std::min(440, width_ - 20);
    constexpr int h = 196;
    const int x = (width_ - w) / 2, y = (height_ - h) / 2;
    return {{x, y, w, h}, {x + 16, y + 120, w / 2 - 24, 58},
            {x + w / 2 + 8, y + 120, w - w / 2 - 24, 58}};
}

void Framebuffer::draw_power_prompt(int seconds_left, bool failed) {
    clear();
    const auto layout = power_layout();
    const auto panel = layout.panel;
    fill_panel_rect(panel, 0x20262d);
    fill_panel_rect({panel.x, panel.y, panel.w, 2}, 0xff8383);
    draw_panel_label({panel.x, panel.y + 12, panel.w, 38}, "SHUT DOWN PI?", 0xf4f7fa);
    const auto status = failed ? std::string("SHUTDOWN FAILED") :
        "CLOSES IN " + std::to_string(seconds_left) + " S";
    draw_panel_label({panel.x, panel.y + 58, panel.w, 38}, status, 0xffb62f);
    fill_panel_rect(layout.cancel, 0x36424e);
    fill_panel_rect(layout.shutdown, 0x882f3e);
    draw_panel_label(layout.cancel, "CANCEL", 0xf4f7fa);
    draw_panel_label(layout.shutdown, "SHUT DOWN", 0xf4f7fa);
}

SettingsLayout Framebuffer::settings_layout() const noexcept {
    const int w = std::min(440, width_ - 20);
    constexpr int h = 230;
    const int x = (width_ - w) / 2, y = (height_ - h) / 2;
    return {{x, y, w, h}, {x + 16, y + 63, 54, 52},
            {x + w - 70, y + 63, 54, 52},
            {x + 16, y + 160, w / 2 - 24, 52},
            {x + w / 2 + 8, y + 160, w - w / 2 - 24, 52}};
}

void Framebuffer::fill_panel_rect(Rect rect, std::uint32_t rgb) {
    const auto color = pack(rgb);
    for (int x = 0; x < rect.w; ++x) {
        write_pixel(row_.data() + x * pixel_bytes_, pixel_bytes_, color);
    }
    for (int y = rect.y; y < rect.y + rect.h; ++y) {
        std::memcpy(at(rect.x, y), row_.data(), static_cast<std::size_t>(rect.w) * pixel_bytes_);
    }
}

void Framebuffer::draw_panel_label(Rect rect, std::string_view text, std::uint32_t rgb) {
    const auto color = pack(rgb);
    const int x = rect.x + (rect.w - tile_width(text)) / 2;
    const int y = rect.y + (rect.h - text_height) / 2;
    text_pixels(text, [&](int dx, int dy) { write_pixel(at(x + dx, y + dy), pixel_bytes_, color); });
}

void Framebuffer::draw_settings(int minutes, bool save_failed) {
    clear();
    const auto layout = settings_layout();
    constexpr auto light = 0xf4f7fa;
    const auto panel = layout.panel;
    fill_panel_rect(panel, 0x20262d);
    fill_panel_rect({panel.x, panel.y, panel.w, 2}, 0x70879a);
    draw_panel_label({panel.x, panel.y + 12, panel.w, 38}, "SCREEN TIMEOUT", light);
    for (const auto button : {layout.minus, layout.plus, layout.off, layout.done}) {
        fill_panel_rect(button, 0x36424e);
    }
    draw_panel_label(layout.minus, "-", minutes > 0 ? light : 0x8a98a5);
    draw_panel_label(layout.plus, "+", minutes < 30 ? light : 0x8a98a5);
    const auto value = minutes == 0 ? std::string("OFF") : std::to_string(minutes) + " MIN";
    draw_panel_label({panel.x, layout.minus.y, panel.w, layout.minus.h}, value, 0xffb62f);
    if (save_failed) {
        draw_panel_label({panel.x, panel.y + 123, panel.w, 27}, "SAVE FAILED", 0xff8383);
    }
    draw_panel_label(layout.off, "OFF", light);
    draw_panel_label(layout.done, "DONE", light);
}

void Framebuffer::prepare_reset_button() {
    if (!reset_pixels_.empty()) return;
    render_button(reset_pixels_, pixel_bytes_, ButtonIcon::reset,
                  [this](auto color) { return pack(color); });
}

void Framebuffer::prepare_lock_button(bool locked) {
    const int state = locked ? 1 : 0;
    if (!lock_pixels_.empty() && lock_prepared_ == state) return;
    lock_prepared_ = state;
    render_button(lock_pixels_, pixel_bytes_, locked ? ButtonIcon::locked : ButtonIcon::unlocked,
                  [this](auto color) { return pack(color); });
}

void Framebuffer::prepare_button(CaptureFeedback feedback) {
    if (!button_pixels_.empty() && feedback == button_feedback_) return;
    button_feedback_ = feedback;
    const auto icon = feedback == CaptureFeedback::saved ? ButtonIcon::saved :
                      feedback == CaptureFeedback::failed ? ButtonIcon::failed : ButtonIcon::camera;
    render_button(button_pixels_, pixel_bytes_, icon, [this](auto color) { return pack(color); });
}

void Framebuffer::save_screenshot(const std::string& directory, std::string& path) const {
    namespace fs = std::filesystem;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width_) * height_ * 3);
    auto* rgb = pixels.data();
    for (int y = 0; y < height_; ++y) {
        const auto* src = at(0, y);
        for (int x = 0; x < width_; ++x, src += pixel_bytes_) {
            std::uint32_t packed;
            if (pixel_bytes_ == 2) {
                std::uint16_t pixel;
                std::memcpy(&pixel, src, sizeof(pixel));
                packed = pixel;
            } else {
                std::memcpy(&packed, src, sizeof(packed));
            }
            for (const auto& channel : rgb_) {
                const auto value = (packed >> channel.shift) & channel.max;
                *rgb++ = static_cast<std::uint8_t>((std::uint64_t{value} * 255 + channel.max / 2) /
                                                   channel.max);
            }
        }
    }

    fs::create_directories(directory);
    char timestamp[24] = {};
    const auto now = std::time(nullptr);
    std::tm local{};
    if (::localtime_r(&now, &local)) std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S-", &local);
    auto temp = (fs::path(directory) / (std::string("screenshot-") + timestamp + "XXXXXX.tmp")).string();
    const int fd = ::mkstemps(temp.data(), 4);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), temp);
    path = temp.substr(0, temp.size() - 4) + ".png";
    if (::fchmod(fd, 0660) < 0) {
        const int code = errno;
        ::close(fd);
        ::unlink(temp.c_str());
        throw std::system_error(code, std::generic_category(), temp);
    }
    FILE* file = ::fdopen(fd, "wb");
    if (!file) {
        const int code = errno;
        ::close(fd);
        ::unlink(temp.c_str());
        throw std::system_error(code, std::generic_category(), temp);
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width_;
    image.height = height_;
    image.format = PNG_FORMAT_RGB;
    const bool written = png_image_write_to_stdio(&image, file, 0, pixels.data(), 0, nullptr) != 0;
    const int close_result = std::fclose(file);
    const int code = errno;
    png_image_free(&image);
    if (!written || close_result != 0 || ::rename(temp.c_str(), path.c_str()) != 0) {
        const int file_error = close_result != 0 ? code : errno;
        ::unlink(temp.c_str());
        if (!written) throw std::runtime_error(image.message);
        throw std::system_error(file_error, std::generic_category(), path);
    }
}

void Framebuffer::message(std::string_view text) {
    clear();
    text = clip_text(text, width_);
    if (text.empty() || height_ < text_height) return;
    const int w = tile_width(text);
    const int dx = (width_ - w) / 2;
    const int dy = (height_ - text_height) / 2;
    const auto color = pack(0x00cccccc);
    text_pixels(text, [&](int x, int y) { write_pixel(at(dx + x, dy + y), pixel_bytes_, color); });
}

}  // namespace microcam
