#include "touch.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <glob.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace microcam {
namespace {

using namespace std::chrono_literals;
constexpr auto double_tap_time = 550ms;
constexpr auto max_tap_time = 450ms;
constexpr auto max_button_tap_time = 650ms;
constexpr double double_tap_distance = 60;
constexpr double max_tap_displacement = 12;
constexpr double max_button_displacement = 32;
constexpr int max_slots = 256;

bool has_event(int fd, unsigned type, unsigned code) {
    // The kernel returns a bitmap of native unsigned longs.
    constexpr unsigned bits_per_word = sizeof(unsigned long) * 8;
    std::array<unsigned long, KEY_MAX / bits_per_word + 1> bits{};
    return ioctl(fd, EVIOCGBIT(type, sizeof(bits)), bits.data()) >= 0 &&
           (bits[code / bits_per_word] & (1ul << (code % bits_per_word)));
}

bool read_axis(int fd, unsigned code, input_absinfo& axis) {
    return ioctl(fd, EVIOCGABS(code), &axis) == 0 && axis.minimum < axis.maximum;
}

std::string lowercase(std::string name) {
    for (char& ch : name) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return name;
}

double distance_sq(double dx, double dy) { return dx * dx + dy * dy; }

} // namespace

Viewport::Viewport(int screen_w, int screen_h, bool fill)
    : screen_w_(std::max(screen_w, 1)), screen_h_(std::max(screen_h, 1)), fill_(fill) {}

void Viewport::set_camera(int w, int h) {
    if (w < 1 || h < 1 || (w == camera_w_ && h == camera_h_)) return;
    camera_w_ = w;
    camera_h_ = h;
    zoom_ = 1;
    center();
}

Viewport::Size Viewport::crop_size() const {
    if (!fill_) return {std::max(camera_w_ / zoom_, 1.0), std::max(camera_h_ / zoom_, 1.0)};
    const double aspect = static_cast<double>(screen_w_) / screen_h_;
    const double width = std::min<double>(camera_w_, camera_h_ * aspect);
    return {std::max(width / zoom_, 1.0), std::max(width / aspect / zoom_, 1.0)};
}

void Viewport::center() {
    const auto crop = crop_size();
    x_ = (camera_w_ - crop.w) / 2;
    y_ = (camera_h_ - crop.h) / 2;
}

void Viewport::clamp_origin() {
    const auto crop = crop_size();
    x_ = std::clamp(x_, 0.0, std::max(camera_w_ - crop.w, 0.0));
    y_ = std::clamp(y_, 0.0, std::max(camera_h_ - crop.h, 0.0));
}

Rect Viewport::source() const {
    const auto crop = crop_size();
    const int w = std::clamp(static_cast<int>(std::lround(crop.w)), 1, camera_w_);
    const int h = std::clamp(static_cast<int>(std::lround(crop.h)), 1, camera_h_);
    return {std::clamp(static_cast<int>(std::lround(x_)), 0, camera_w_ - w),
            std::clamp(static_cast<int>(std::lround(y_)), 0, camera_h_ - h), w, h};
}

Rect Viewport::destination() const {
    if (fill_) return {0, 0, screen_w_, screen_h_};
    const double scale = std::min(static_cast<double>(screen_w_) / camera_w_,
                                  static_cast<double>(screen_h_) / camera_h_);
    const int w = std::clamp(static_cast<int>(std::lround(camera_w_ * scale)), 1, screen_w_);
    const int h = std::clamp(static_cast<int>(std::lround(camera_h_ * scale)), 1, screen_h_);
    return {(screen_w_ - w) / 2, (screen_h_ - h) / 2, w, h};
}

bool Viewport::reset_zoom() {
    if (zoom_ == 1) return false;
    zoom_ = 1;
    center();
    return true;
}

bool Viewport::cycle_zoom(double x, double y) {
    if (locked_) return false;
    if (zoom_ == 5) return reset_zoom();
    const auto dst = destination();
    const auto crop = crop_size();
    const double fx = std::clamp((x - dst.x) / dst.w, 0.0, 1.0);
    const double fy = std::clamp((y - dst.y) / dst.h, 0.0, 1.0);
    zoom_ = zoom_ == 1 ? 2.5 : 5;
    const auto new_crop = crop_size();
    x_ += fx * (crop.w - new_crop.w);
    y_ += fy * (crop.h - new_crop.h);
    clamp_origin();
    return true;
}

TouchDevice::~TouchDevice() { close(); }

bool TouchDevice::open(const std::string& path, const std::string& name_hint) {
    close();
    const std::string hint = lowercase(name_hint);
    int best_score = 0;
    auto consider = [&](const char* candidate) {
        const int fd = ::open(candidate, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return;

        char name[256] = {};
        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        input_absinfo x{}, y{}, slot{};
        const bool mt = has_event(fd, EV_ABS, ABS_MT_SLOT) &&
                        has_event(fd, EV_ABS, ABS_MT_TRACKING_ID) &&
                        ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot) == 0 &&
                        slot.minimum == 0 && slot.maximum >= 0 && slot.maximum < max_slots &&
                        read_axis(fd, ABS_MT_POSITION_X, x) && read_axis(fd, ABS_MT_POSITION_Y, y);
        const bool single = !mt && has_event(fd, EV_KEY, BTN_TOUCH) &&
                            read_axis(fd, ABS_X, x) && read_axis(fd, ABS_Y, y);
        if (!mt && !single) {
            ::close(fd);
            return;
        }

        const bool named = !hint.empty() && lowercase(name).find(hint) != std::string::npos;
        const int score = (named ? 2 : 0) + (mt ? 2 : 1);
        if (score <= best_score) {
            ::close(fd);
            return;
        }
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
        path_ = candidate;
        name_ = *name ? name : candidate;
        multitouch_ = mt;
        primary_slot_only_ = mt && lowercase(name_).find("waveshare") != std::string::npos;
        x_axis_ = {x.minimum, x.maximum};
        y_axis_ = {y.minimum, y.maximum};
        slots_.assign(mt ? slot.maximum + 1 : 1, Slot{});
        best_score = score;
    };

    if (!path.empty()) {
        consider(path.c_str());
    } else {
        glob_t nodes{};
        if (glob("/dev/input/event*", 0, nullptr, &nodes) == 0) {
            for (std::size_t i = 0; i < nodes.gl_pathc && best_score < 4; ++i) {
                consider(nodes.gl_pathv[i]);
            }
        }
        globfree(&nodes);
    }
    if (fd_ < 0) return false;

    int clock_id = CLOCK_MONOTONIC;
    monotonic_events_ = ioctl(fd_, EVIOCSCLOCKID, &clock_id) == 0;
    resync();
    return true;
}

void TouchDevice::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    path_.clear();
    name_.clear();
    dropped_ = blocked_ = false;
    slots_.clear();
    contact_.reset();
    last_tap_.reset();
}

void TouchDevice::resync() {
    // A dropped frame means slot and key transitions may have been lost.
    // Query the kernel state and wait for the current gesture to end.
    for (auto& slot : slots_) slot = {};
    current_slot_ = 0;
    if (multitouch_) {
        input_absinfo slot{};
        if (ioctl(fd_, EVIOCGABS(ABS_MT_SLOT), &slot) == 0 && slot.value >= 0 &&
            static_cast<std::size_t>(slot.value) < slots_.size()) {
            current_slot_ = slot.value;
        }
        std::vector<std::int32_t> values(slots_.size() + 1);
        auto fetch = [&](int axis) {
            values[0] = axis;
            return ioctl(fd_, EVIOCGMTSLOTS(values.size() * sizeof(values[0])), values.data()) == 0;
        };
        if (fetch(ABS_MT_TRACKING_ID)) {
            for (std::size_t i = 0; i < slots_.size(); ++i) slots_[i].id = values[i + 1];
        }
        if (fetch(ABS_MT_POSITION_X)) {
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                slots_[i].x = values[i + 1];
                slots_[i].have_x = true;
            }
        }
        if (fetch(ABS_MT_POSITION_Y)) {
            for (std::size_t i = 0; i < slots_.size(); ++i) {
                slots_[i].y = values[i + 1];
                slots_[i].have_y = true;
            }
        }
    } else {
        constexpr unsigned bits_per_word = sizeof(unsigned long) * 8;
        std::array<unsigned long, KEY_MAX / bits_per_word + 1> keys{};
        if (ioctl(fd_, EVIOCGKEY(sizeof(keys)), keys.data()) >= 0 &&
            (keys[BTN_TOUCH / bits_per_word] & (1ul << (BTN_TOUCH % bits_per_word)))) {
            slots_[0].id = 0;
        }
        input_absinfo x{}, y{};
        slots_[0].have_x = read_axis(fd_, ABS_X, x);
        slots_[0].have_y = read_axis(fd_, ABS_Y, y);
        slots_[0].x = x.value;
        slots_[0].y = y.value;
    }
    contact_.reset();
    last_tap_.reset();
    blocked_ = std::any_of(slots_.begin(), slots_.end(), [](const Slot& s) { return s.id >= 0; });
}

TouchUpdate TouchDevice::read_events(Viewport& view, bool sleeping, Rect screenshot_button, Rect reset_button,
                                     Rect lock_button, Rect settings_button, bool settings_open) {
    TouchUpdate update;
    if (fd_ < 0) return update;

    input_event events[64];
    for (int batch = 0; batch < 16; ++batch) {
        const ssize_t bytes = ::read(fd_, events, sizeof(events));
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (bytes <= 0 || bytes % sizeof(input_event) != 0) {
            close();
            break;
        }

        for (std::size_t i = 0; i < static_cast<std::size_t>(bytes) / sizeof(input_event); ++i) {
            const auto& ev = events[i];
            if (ev.type == EV_SYN) {
                if (ev.code == SYN_DROPPED) dropped_ = true;
                if (ev.code != SYN_REPORT) continue;
                if (dropped_) {
                    dropped_ = false;
                    resync();
                    continue;
                }
                auto now = Clock::now();
                if (monotonic_events_) {
                    using namespace std::chrono;
                    const auto stamp = seconds(ev.input_event_sec) + microseconds(ev.input_event_usec);
                    now = Clock::time_point(duration_cast<Clock::duration>(stamp));
                }
                process_report(now, view, sleeping && !update.activity, screenshot_button, reset_button,
                               lock_button, settings_button, settings_open, update);
                continue;
            }
            if (dropped_) continue;
            if (multitouch_ && ev.type == EV_ABS && ev.code == ABS_MT_SLOT) {
                current_slot_ = ev.value;
            }
            if (current_slot_ < 0 || static_cast<std::size_t>(current_slot_) >= slots_.size()) continue;
            auto& slot = slots_[current_slot_];
            if (ev.type == EV_ABS) {
                if (ev.code == (multitouch_ ? ABS_MT_POSITION_X : ABS_X)) {
                    slot.x = ev.value;
                    slot.have_x = true;
                } else if (ev.code == (multitouch_ ? ABS_MT_POSITION_Y : ABS_Y)) {
                    slot.y = ev.value;
                    slot.have_y = true;
                } else if (multitouch_ && ev.code == ABS_MT_TRACKING_ID) {
                    // Keep the positions when a contact ends. evdev omits
                    // coordinates on a later tap at the same point.
                    slot.id = ev.value;
                }
            } else if (!multitouch_ && ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value != 2) {
                slot.id = ev.value == 1 ? 0 : -1;
            }
        }
    }
    return update;
}

TouchDevice::Point TouchDevice::map(const Slot& slot, const Viewport& view) const {
    auto scale = [](int value, Axis axis, int pixels) {
        const double fraction = (static_cast<double>(value) - axis.min) /
                                (static_cast<double>(axis.max) - axis.min);
        return std::clamp(fraction, 0.0, 1.0) * (pixels - 1);
    };
    return {scale(slot.x, x_axis_, view.screen_width()), scale(slot.y, y_axis_, view.screen_height())};
}

void TouchDevice::block_gestures() {
    blocked_ = true;
    contact_.reset();
    last_tap_.reset();
}

void TouchDevice::process_report(Clock::time_point now, Viewport& view, bool sleeping, Rect screenshot_button,
                                 Rect reset_button, Rect lock_button, Rect settings_button,
                                 bool settings_open, TouchUpdate& update) {
    unsigned active = 0, current = 0;
    for (unsigned i = 0; i < slots_.size(); ++i) {
        // This WaveShare panel reports each physical touch in slots 0 and 1.
        // Slot 0 is consistently the primary contact; treating the duplicate
        // as a second finger makes every tap look like a multitouch gesture.
        if (primary_slot_only_ && i != 0) continue;
        if (slots_[i].id >= 0) {
            ++active;
            current = i;
        }
    }
    if (sleeping && active != 0) {
        const auto& slot = slots_[current];
        std::cerr << "[touch] wake input: contacts=" << active << ", slot=" << current
                  << ", tracking_id=" << slot.id;
        if (slot.have_x && slot.have_y) {
            const auto p = map(slot, view);
            std::cerr << ", position=" << p.x << ',' << p.y;
        }
        std::cerr << '\n';
    }
    if (active > 1 || (active != 0 && sleeping)) {
        update.activity = true;
        if (active > 1) {
            std::cerr << "[touch] multiple contacts, slot ids";
            for (const auto& slot : slots_) std::cerr << ' ' << slot.id;
            std::cerr << '\n';
        }
        block_gestures();
    }
    if (blocked_) {
        if (active == 0) {
            std::cerr << "[touch] all contacts lifted\n";
            blocked_ = false;
        }
        return;
    }

    if (active == 1) {
        const auto& slot = slots_[current];
        if (!slot.have_x || !slot.have_y) return;
        const Point position = map(slot, view);
        if (!contact_) {
            std::cerr << "[touch] contact start slot=" << current << " id=" << slot.id
                      << " position=" << position.x << ',' << position.y << '\n';
            contact_ = Contact{current, slot.id, position, position, now};
            update.activity = true;
        } else if (contact_->slot == current && contact_->id == slot.id) {
            move_contact(position, update);
        } else {
            // A replacement in the same report is ambiguous. Wait for lift.
            update.activity = true;
            block_gestures();
        }
        return;
    }

    if (!contact_) return;
    // Some devices include the final coordinates with the release event.
    move_contact(map(slots_[contact_->slot], view), update);
    const auto released = *contact_;
    contact_.reset();
    const auto duration = now - released.began;
    const auto point = released.position;
    const bool button_tap = duration <= max_button_tap_time &&
        released.max_displacement_sq <= max_button_displacement * max_button_displacement;
    if (settings_open || update.settings) {
        last_tap_.reset();
        if (settings_open && button_tap && !update.settings_tap) {
            update.settings_tap.emplace(point.x, point.y);
        }
        return;
    }
    const bool gear_start = settings_button.contains(released.start.x, released.start.y);
    const bool gear_end = settings_button.contains(point.x, point.y);
    if (gear_start || gear_end) {
        last_tap_.reset();
        if (button_tap && gear_start && gear_end) update.settings = true;
        return;
    }
    const bool reset_start = reset_button.contains(released.start.x, released.start.y);
    const bool reset_end = reset_button.contains(point.x, point.y);
    const bool screenshot_start = screenshot_button.contains(released.start.x, released.start.y);
    const bool screenshot_end = screenshot_button.contains(point.x, point.y);
    const bool lock_start = lock_button.contains(released.start.x, released.start.y);
    const bool lock_end = lock_button.contains(point.x, point.y);
    if (reset_start || reset_end || screenshot_start || screenshot_end || lock_start || lock_end) {
        last_tap_.reset();
        if (button_tap) {
            if (lock_start && lock_end) {
                view.toggle_lock();
                update.lock_toggled = true;
                update.changed = true;
            } else {
                if (reset_start && reset_end) update.changed |= view.reset_zoom();
                if (screenshot_start && screenshot_end) update.screenshot = true;
            }
        }
        return;
    }
    // While zoom is locked, ignore every other gesture (double-tap zoom).
    if (view.locked()) {
        last_tap_.reset();
        return;
    }
    if (released.max_displacement_sq > max_tap_displacement * max_tap_displacement ||
        duration > max_tap_time) {
        std::cerr << "[touch] tap rejected, movement=" << std::sqrt(released.max_displacement_sq)
                  << "px duration=" << std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()
                  << "ms\n";
        last_tap_.reset();
        return;
    }
    if (last_tap_ && now - last_tap_->time < double_tap_time &&
        distance_sq(point.x - last_tap_->position.x, point.y - last_tap_->position.y) <
            double_tap_distance * double_tap_distance) {
        std::cerr << "[touch] double tap accepted\n";
        update.changed |= view.cycle_zoom(point.x, point.y);
        last_tap_.reset();
    } else {
        if (last_tap_) {
            std::cerr << "[touch] second tap rejected, interval="
                      << std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tap_->time).count()
                      << "ms distance=" << std::sqrt(distance_sq(point.x - last_tap_->position.x,
                                                                  point.y - last_tap_->position.y))
                      << "px\n";
        } else {
            std::cerr << "[touch] first tap\n";
        }
        last_tap_ = Tap{point, now};
    }
}

void TouchDevice::move_contact(Point position, TouchUpdate& update) {
    auto& contact = *contact_;
    if (position.x == contact.position.x && position.y == contact.position.y) return;
    contact.position = position;
    contact.max_displacement_sq = std::max(contact.max_displacement_sq,
        distance_sq(position.x - contact.start.x, position.y - contact.start.y));
    update.activity = true;
}

} // namespace microcam
