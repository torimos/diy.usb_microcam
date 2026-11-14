#include "camera.hpp"
#include "display.hpp"
#include "platform.hpp"
#include "touch.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using namespace microcam;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct Options {
    CameraOptions camera;
    std::string framebuffer = "/dev/fb0";
    std::string screenshots = "screenshots";
    std::string settings_file = ".microcam-timeout";
    std::string touch_path;
    std::string touch_name = "waveshare";
    std::string gpio_chip = "/dev/gpiochip0";
    unsigned gpio_line = 26;
    std::chrono::seconds idle{300};
    bool backlight = true;
    bool vt = true;
    bool fill = false;
    bool osd = true;
};

void usage(std::ostream& out) {
    out << R"(Usage: cam_viewer [options]
Full-screen USB camera viewer for a Linux framebuffer and evdev touchscreen.

  --device PATH       V4L2 capture node (default: find a USB camera)
  --size WIDTHxHEIGHT Request capture size (default: 640x480)
  --fps N             Request capture rate (default: 30)
  --fixed-exposure-fps Prevent auto exposure from lowering the camera frame rate
  --fb PATH           Framebuffer (default: /dev/fb0)
  --screenshots DIR   Screenshot folder (default: screenshots)
  --touch PATH        Touch event node; overrides name matching
  --touch-name TEXT   Case-insensitive device name match (default: waveshare)
                      Use an empty string for any compatible touchscreen
  --idle SECONDS      Initial backlight timeout before a setting is saved (default: 300)
  --settings-file PATH File used to save the screen timeout
  --gpio-chip PATH    GPIO chip (default: /dev/gpiochip0)
  --gpio-line N       Active-low line offset (default: 26, BCM GPIO 26 on Pi 3)
  --no-backlight      Leave the GPIO untouched
  --fill              Fill the screen by cropping the picture
  --no-osd            Hide zoom, camera and FPS overlay
  --no-vt             Don't take over the virtual console and its framebuffer
  -h, --help          Show this help

Double-tap to cycle zoom 1x / 2.5x / 5x. Zoom starts locked; tap the lock icon to unlock.
Tap to wake the backlight. Ctrl+C stops the viewer.
)";
}

int number(std::string_view arg, int min, int max, std::string_view option) {
    int value = 0;
    const auto result = std::from_chars(arg.data(), arg.data() + arg.size(), value);
    if (result.ec != std::errc{} || result.ptr != arg.data() + arg.size() || value < min || value > max) {
        throw std::invalid_argument(std::string(option) + " expects an integer from " +
                                    std::to_string(min) + " to " + std::to_string(max));
    }
    return value;
}

Options parse(int argc, char* argv[], bool& help) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        const auto next = [&]() -> std::string_view {
            if (++i == argc || std::string_view(argv[i]).substr(0, 2) == "--") {
                throw std::invalid_argument(std::string(option) + " requires a value");
            }
            return argv[i];
        };
        if (option == "-h" || option == "--help") { help = true; return opts; }
        if (option == "--device") opts.camera.device = next();
        else if (option == "--size") {
            const auto size = next();
            const auto x = size.find('x');
            if (x == std::string_view::npos) throw std::invalid_argument("--size expects WIDTHxHEIGHT");
            opts.camera.width = number(size.substr(0, x), 1, 8192, "--size width");
            opts.camera.height = number(size.substr(x + 1), 1, 8192, "--size height");
        } else if (option == "--fps") opts.camera.fps = number(next(), 1, 240, option);
        else if (option == "--fixed-exposure-fps") opts.camera.fixed_exposure_fps = true;
        else if (option == "--fb") opts.framebuffer = next();
        else if (option == "--screenshots") opts.screenshots = next();
        else if (option == "--touch") opts.touch_path = next();
        else if (option == "--touch-name") opts.touch_name = next();
        else if (option == "--settings-file") opts.settings_file = next();
        else if (option == "--idle") opts.idle = std::chrono::seconds(number(next(), 0, 86400, option));
        else if (option == "--gpio-chip") opts.gpio_chip = next();
        else if (option == "--gpio-line") opts.gpio_line = number(next(), 0, 65535, option);
        else if (option == "--no-backlight") opts.backlight = false;
        else if (option == "--fill") opts.fill = true;
        else if (option == "--no-osd") opts.osd = false;
        else if (option == "--no-vt") opts.vt = false;
        else throw std::invalid_argument("unknown option: " + std::string(option));
    }
    return opts;
}

std::chrono::seconds load_timeout(const Options& opts) {
    std::ifstream input(opts.settings_file);
    int minutes;
    std::string extra;
    if (input >> minutes && minutes >= 0 && minutes <= 30 && !(input >> extra)) {
        return std::chrono::minutes(minutes);
    }
    return opts.idle;
}

bool save_timeout(const std::string& path, int minutes) {
    std::string temporary = path + ".XXXXXX";
    const int fd = ::mkstemp(temporary.data());
    if (fd < 0) return false;
    const auto value = std::to_string(minutes) + '\n';
    const auto written = ::write(fd, value.data(), value.size());
    const int closed = ::close(fd);
    if (written == static_cast<ssize_t>(value.size()) && closed == 0 &&
        ::rename(temporary.c_str(), path.c_str()) == 0) return true;
    ::unlink(temporary.c_str());
    return false;
}

int run(const Options& opts) {
    StopSignal stop;
    Console console(opts.vt);
    Framebuffer display(opts.framebuffer);
    if (!console.warning().empty()) std::cerr << "[console] " << console.warning() << '\n';
    display.clear();  // Erase boot and login text left on the screen.
    Backlight backlight;
    std::string error;
    auto idle_timeout = load_timeout(opts);
    bool idle_enabled = opts.backlight;
    if (opts.backlight && !backlight.open(opts.gpio_chip, opts.gpio_line, error)) {
        std::cerr << "[backlight] " << error << "; continuing without backlight control\n";
        idle_enabled = false;
    }
    std::cerr << "[display] " << opts.framebuffer << ' ' << display.width() << 'x' << display.height() << '\n';

    Camera camera;
    Frame frame;
    TouchDevice touch;
    Viewport view(display.width(), display.height(), opts.fill);
    view.set_locked(true);  // Start with zoom locked; the lock icon unlocks it.
    auto now = Clock::now();
    auto next_camera = now, next_touch = now, last_activity = now;
    auto camera_deadline = now, rate_start = now;
    auto feedback_until = now, flash_until = now, next_screenshot = now;
    auto feedback = Framebuffer::CaptureFeedback::ready;
    bool capture_flash = false;
    bool settings_open = false, settings_save_failed = false;
    int settings_minutes = 0;
    bool asleep = false, redraw = true, have_frame = false, fresh_frame = false, touch_missing = false;
    unsigned shown_frames = 0;
    double fps = 0;
    std::string camera_error;

    const auto reset_rate = [&] {
        rate_start = Clock::now();
        shown_frames = 0;
        fps = 0;
    };
    const auto wake = [&] {
        if (!asleep) return;
        if (!backlight.set(true, error)) {
            std::cerr << "[backlight] could not turn on: " << error << '\n';
            idle_enabled = false;
        }
        asleep = false;
        redraw = true;
        reset_rate();
    };
    const auto lose_camera = [&](const std::string& reason) {
        std::cerr << "[camera] lost " << camera.path() << ": " << reason << "; will retry\n";
        camera.close();
        next_camera = Clock::now() + 2s;
        have_frame = false;
        fresh_frame = false;
        redraw = true;
    };

    for (;;) {
        now = Clock::now();
        if (capture_flash && now >= flash_until) {
            capture_flash = false;
            redraw = true;
        }
        if (feedback != Framebuffer::CaptureFeedback::ready && now >= feedback_until) {
            feedback = Framebuffer::CaptureFeedback::ready;
            redraw = true;
        }
        if (touch.fd() < 0 && now >= next_touch) {
            if (touch.open(opts.touch_path, opts.touch_name)) {
                std::cerr << "[touch] " << touch.name() << " (" << touch.path() << ")\n";
                touch_missing = false;
                last_activity = Clock::now();
            } else if (!touch_missing) {
                std::cerr << "[touch] no accessible matching touchscreen; retrying (see --touch and --touch-name)\n";
                touch_missing = true;
            }
            next_touch = Clock::now() + 2s;
        }
        if (camera.fd() < 0 && now >= next_camera) {
            error.clear();
            if (camera.open(opts.camera, error)) {
                std::cerr << "[camera] " << camera.name() << " (" << camera.path() << ") " <<
                    camera.width() << 'x' << camera.height() << ' ' << camera.format() << '\n';
                camera_error.clear();
                camera_deadline = Clock::now() + 5s;
                reset_rate();
                redraw = true;
            } else if (error != camera_error) {
                std::cerr << "[camera] " << error << "; retrying\n";
                camera_error = error;
            }
            next_camera = Clock::now() + 2s;
        }
        now = Clock::now();
        if (camera.fd() >= 0 && now >= camera_deadline) lose_camera("no usable frames for 5 seconds");
        if (!asleep && !settings_open && idle_enabled && idle_timeout > 0s &&
            touch.fd() >= 0 && now - last_activity >= idle_timeout) {
            if (backlight.set(false, error)) {
                asleep = true;
            } else {
                std::cerr << "[backlight] could not turn off: " << error << '\n';
                idle_enabled = false;
            }
        }

        if (!asleep && redraw) {
            if (settings_open) {
                display.draw_settings(settings_minutes, settings_save_failed);
            } else if (have_frame) {
                const auto src = view.source(), dst = view.destination();
                if (fresh_frame) ++shown_frames;
                fresh_frame = false;
                const std::chrono::duration<double> elapsed = Clock::now() - rate_start;
                if (elapsed >= 1s) {
                    fps = shown_frames / elapsed.count();
                    rate_start = Clock::now();
                    shown_frames = 0;
                }
                char label[128];
                std::string_view status;
                if (opts.osd) {
                    std::snprintf(label, sizeof(label), "ZOOM %.1fX\nFPS %.1f", view.zoom(), fps);
                    status = label;
                }
                display.draw(frame.pixels.data(), frame.width, frame.height,
                             src.x, src.y, src.w, src.h, dst.x, dst.y, dst.w, dst.h,
                             status, feedback, capture_flash, view.locked());
            } else {
                display.message(camera.fd() < 0 ? "NO CAMERA - WAITING" : "WAITING FOR VIDEO");
            }
            redraw = false;
        }

        auto deadline = camera.fd() < 0 ? next_camera : camera_deadline;
        if (feedback != Framebuffer::CaptureFeedback::ready) deadline = std::min(deadline, feedback_until);
        if (capture_flash) deadline = std::min(deadline, flash_until);
        if (touch.fd() < 0) deadline = std::min(deadline, next_touch);
        else if (!asleep && !settings_open && idle_enabled && idle_timeout > 0s) {
            deadline = std::min(deadline, last_activity + idle_timeout);
        }
        const auto wait = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        pollfd fds[] = {{stop.fd(), POLLIN, 0}, {touch.fd(), POLLIN, 0}, {camera.fd(), POLLIN, 0}};
        if (poll(fds, 3, static_cast<int>(std::max(wait, 0ms).count())) < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
        }
        if (fds[0].revents != 0) break;

        constexpr short gone = POLLERR | POLLHUP | POLLNVAL;
        if (fds[1].revents & (POLLIN | gone)) {
            if (fds[1].revents & POLLIN) {
                const double zoom_before = view.zoom();
                const bool buttons_visible = have_frame && !asleep && !settings_open;
                const auto update = touch.read_events(view, asleep,
                    buttons_visible ? display.screenshot_touch_area() : Rect{},
                    buttons_visible ? display.reset_touch_area() : Rect{},
                    buttons_visible ? display.lock_touch_area() : Rect{},
                    buttons_visible ? display.settings_touch_area() : Rect{}, settings_open);
                if (update.settings) {
                    settings_open = true;
                    settings_save_failed = false;
                    settings_minutes = std::clamp<int>(
                        std::chrono::ceil<std::chrono::minutes>(idle_timeout).count(), 0, 30);
                    redraw = true;
                } else if (update.settings_tap) {
                    const auto [x, y] = *update.settings_tap;
                    const auto layout = display.settings_layout();
                    if (layout.minus.contains(x, y)) settings_minutes = std::max(0, settings_minutes - 1);
                    else if (layout.plus.contains(x, y)) settings_minutes = std::min(30, settings_minutes + 1);
                    else if (layout.off.contains(x, y)) settings_minutes = 0;
                    else if (!layout.panel.contains(x, y)) settings_open = false;
                    else if (layout.done.contains(x, y)) {
                        if (save_timeout(opts.settings_file, settings_minutes)) {
                            idle_timeout = std::chrono::minutes(settings_minutes);
                            settings_open = false;
                            settings_save_failed = false;
                            std::cerr << "[settings] screen timeout set to " << settings_minutes << " min\n";
                        } else {
                            std::cerr << "[settings] could not save " << opts.settings_file << '\n';
                            settings_save_failed = true;
                        }
                    }
                    redraw = true;
                }
                if (update.screenshot && Clock::now() >= next_screenshot) {
                    try {
                        std::string path;
                        display.save_screenshot(opts.screenshots, path);
                        std::cerr << "[screenshot] saved " << path << '\n';
                        feedback = Framebuffer::CaptureFeedback::saved;
                        capture_flash = true;
                        flash_until = Clock::now() + 140ms;
                    } catch (const std::exception& e) {
                        std::cerr << "[screenshot] " << e.what() << '\n';
                        feedback = Framebuffer::CaptureFeedback::failed;
                    }
                    const auto completed = Clock::now();
                    feedback_until = completed + 1200ms;
                    next_screenshot = completed + 500ms;
                    redraw = true;
                }
                if (view.zoom() != zoom_before) {
                    std::cerr << "[touch] zoom " << zoom_before << "x -> " << view.zoom() << "x\n";
                }
                if (update.activity) {
                    last_activity = Clock::now();
                    wake();
                }
                redraw = redraw || update.changed;
            }
            if (touch.fd() < 0 || (fds[1].revents & gone)) {
                touch.close();
                std::cerr << "[touch] disconnected; will retry\n";
                next_touch = Clock::now() + 2s;
                wake();
            }
        }
        if (fds[2].revents & gone) {
            lose_camera("device disconnected or stopped streaming");
        } else if (fds[2].revents & POLLIN) {
            error.clear();
            const auto result = camera.latest(frame, !asleep && !settings_open, error);
            if (result == Camera::Read::disconnected) lose_camera(error);
            else if (result == Camera::Read::frame) {
                camera_deadline = Clock::now() + 5s;
                view.set_camera(frame.width, frame.height);
                have_frame = true;
                fresh_frame = true;
                redraw = true;
            } else if (asleep || settings_open) {
                camera_deadline = Clock::now() + 5s;
            }
        }
    }
    display.clear();
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        bool help = false;
        const auto options = parse(argc, argv, help);
        if (help) { usage(std::cout); return 0; }
        return run(options);
    } catch (const std::exception& e) {
        std::cerr << "cam_viewer: " << e.what() << '\n';
        return 1;
    }
}
