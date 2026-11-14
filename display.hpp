#pragma once

#include "rect.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace microcam {

struct SettingsLayout {
    Rect panel, minus, plus, off, done;
};

class Framebuffer {
public:
    enum class CaptureFeedback { ready, saved, failed };
    explicit Framebuffer(const std::string& path);
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

    void clear();

    // B, G, R, X bytes in memory, with no padding between input rows.
    // Source and destination rectangles must fit inside their respective images.
    void draw(const std::uint32_t* pixels, int frame_width, int frame_height,
              int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh,
              std::string_view status, CaptureFeedback feedback, bool capture_flash,
              bool zoom_locked = false);
    Rect screenshot_touch_area() const noexcept;
    Rect reset_touch_area() const noexcept;
    Rect lock_touch_area() const noexcept;
    Rect settings_touch_area() const noexcept;
    SettingsLayout settings_layout() const noexcept;
    void draw_settings(int minutes, bool save_failed);
    void save_screenshot(const std::string& directory, std::string& path) const;
    void message(std::string_view text);

private:
    struct StatusPixel {
        std::size_t offset;
        std::uint32_t color;
    };

    std::uint8_t* at(int x, int y) const noexcept;
    std::uint32_t pack(std::uint32_t bgrx) const noexcept;
    void convert_row(const std::uint32_t* input, int width);
    void prepare_status(std::string_view text);
    Rect screenshot_button() const noexcept;
    Rect reset_button() const noexcept;
    Rect lock_button() const noexcept;
    Rect settings_button() const noexcept;
    void prepare_button(CaptureFeedback feedback);
    void prepare_reset_button();
    void prepare_lock_button(bool locked);

    int fd_ = -1;
    std::uint8_t* mapping_ = nullptr;
    std::size_t mapping_size_ = 0;
    std::uint8_t* visible_ = nullptr;
    std::size_t pitch_ = 0;
    int width_ = 0;
    int height_ = 0;
    int pixel_bytes_ = 0;
    bool native32_ = false;
    bool rgb565_ = false;
    std::uint32_t opaque_ = 0;
    struct Channel { unsigned shift; std::uint32_t max; };
    std::array<Channel, 3> rgb_{};
    std::array<std::uint32_t, 256> red_{}, green_{}, blue_{};

    int dest_x_ = 0, dest_y_ = 0, dest_width_ = 0, dest_height_ = 0;
    int source_width_ = 0, source_height_ = 0;
    std::vector<int> columns_, rows_;
    std::vector<std::uint8_t> row_;
    std::vector<std::uint8_t> black_row_, composite_row_;

    std::string status_text_;
    std::vector<std::vector<StatusPixel>> status_rows_;
    std::vector<std::uint8_t> button_pixels_;
    std::vector<std::uint8_t> reset_pixels_;
    std::vector<std::uint8_t> lock_pixels_, settings_pixels_;
    CaptureFeedback button_feedback_ = CaptureFeedback::ready;
    int lock_prepared_ = -1;  // -1 unset, 0 unlocked, 1 locked
    bool flash_visible_ = false;
};

}  // namespace microcam
