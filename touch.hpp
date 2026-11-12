#pragma once

#include "rect.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace microcam {

class Viewport {
public:
    Viewport(int screen_w, int screen_h, bool fill);

    void set_camera(int w, int h);
    Rect source() const;
    Rect destination() const;
    double zoom() const { return zoom_; }
    int screen_width() const { return screen_w_; }
    int screen_height() const { return screen_h_; }
    bool cycle_zoom(double x, double y);
    bool reset_zoom();

private:
    struct Size { double w, h; };
    Size crop_size() const;
    void center();
    void clamp_origin();

    int screen_w_, screen_h_;
    bool fill_;
    int camera_w_ = 1, camera_h_ = 1;
    double zoom_ = 1, x_ = 0, y_ = 0;
};

struct TouchUpdate {
    bool activity = false;
    bool changed = false;
    bool screenshot = false;
};

class TouchDevice {
public:
    TouchDevice() = default;
    ~TouchDevice();
    TouchDevice(const TouchDevice&) = delete;
    TouchDevice& operator=(const TouchDevice&) = delete;

    // An explicit path selects just that device. Otherwise prefer the name hint.
    bool open(const std::string& path, const std::string& name_hint);
    void close();
    int fd() const { return fd_; }
    const std::string& name() const { return name_; }
    const std::string& path() const { return path_; }
    TouchUpdate read_events(Viewport& view, bool sleeping, Rect screenshot_button = {},
                            Rect reset_button = {});

private:
    using Clock = std::chrono::steady_clock;
    struct Axis { int min = 0, max = 1; };
    struct Slot {
        int id = -1, x = 0, y = 0;
        bool have_x = false, have_y = false;
    };
    struct Point { double x, y; };
    struct Contact {
        unsigned slot;
        int id;
        Point start, position;
        Clock::time_point began;
        double max_displacement_sq = 0;
    };
    struct Tap { Point position; Clock::time_point time; };

    void resync();
    void process_report(Clock::time_point now, Viewport& view, bool sleeping, Rect screenshot_button,
                        Rect reset_button, TouchUpdate& update);
    Point map(const Slot& slot, const Viewport& view) const;
    void move_contact(Point position, TouchUpdate& update);
    void block_gestures();

    int fd_ = -1;
    std::string path_, name_;
    bool multitouch_ = false, primary_slot_only_ = false;
    bool monotonic_events_ = false, dropped_ = false;
    bool blocked_ = false;
    Axis x_axis_, y_axis_;
    int current_slot_ = 0;
    std::vector<Slot> slots_;
    std::optional<Contact> contact_;
    std::optional<Tap> last_tap_;
};

} // namespace microcam
