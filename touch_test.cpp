#include "touch.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <stdexcept>
#include <unistd.h>

namespace {

using microcam::TouchDevice;
using microcam::Viewport;
int input_fd = -1;
bool multitouch = true;
constexpr char path[] = "/dev/input/test-touch";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int mock_open(const char* candidate) {
    if (std::strcmp(candidate, path) == 0) return dup(input_fd);
    errno = ENOENT;
    return -1;
}

int mock_ioctl(unsigned long request, va_list args) {
    const auto size = _IOC_SIZE(request);
    if (request == EVIOCGNAME(size)) {
        return std::snprintf(va_arg(args, char*), size, "Test touchscreen");
    }
    if (request == EVIOCGBIT(EV_ABS, size) || request == EVIOCGBIT(EV_KEY, size)) {
        auto* bits = va_arg(args, unsigned long*);
        std::memset(bits, 0, size);
        auto set = [bits](unsigned code) {
            constexpr unsigned width = sizeof(unsigned long) * 8;
            bits[code / width] |= 1ul << (code % width);
        };
        if (multitouch && request == EVIOCGBIT(EV_ABS, size)) {
            set(ABS_MT_SLOT);
            set(ABS_MT_TRACKING_ID);
        }
        if (!multitouch && request == EVIOCGBIT(EV_KEY, size)) set(BTN_TOUCH);
        return 0;
    }
    if (request == EVIOCGABS(ABS_MT_SLOT)) {
        *va_arg(args, input_absinfo*) = {0, 0, 1, 0, 0, 0};
        return 0;
    }
    if (request == EVIOCGABS(ABS_X) || request == EVIOCGABS(ABS_MT_POSITION_X)) {
        *va_arg(args, input_absinfo*) = {0, 0, 1023, 0, 0, 0};
        return 0;
    }
    if (request == EVIOCGABS(ABS_Y) || request == EVIOCGABS(ABS_MT_POSITION_Y)) {
        *va_arg(args, input_absinfo*) = {0, 0, 599, 0, 0, 0};
        return 0;
    }
    if (request == EVIOCGMTSLOTS(3 * sizeof(int))) {
        auto* values = va_arg(args, int*);
        values[1] = values[2] = values[0] == ABS_MT_TRACKING_ID ? -1 : 0;
        return 0;
    }
    if (request == EVIOCGKEY(size)) {
        std::memset(va_arg(args, unsigned long*), 0, size);
        return 0;
    }
    if (request == EVIOCSCLOCKID) return 0;
    errno = EINVAL;
    return -1;
}

class EventPipe {
public:
    EventPipe() {
        int fds[2];
        require(pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0, "create test evdev pipe");
        input_fd = fds[0];
        output_fd_ = fds[1];
    }

    ~EventPipe() {
        close(input_fd);
        close(output_fd_);
        input_fd = -1;
    }

    void emit(int ms, unsigned short type, unsigned short code, int value) {
        input_event ev{};
        ev.input_event_sec = ms / 1000;
        ev.input_event_usec = (ms % 1000) * 1000;
        ev.type = type;
        ev.code = code;
        ev.value = value;
        require(write(output_fd_, &ev, sizeof(ev)) == sizeof(ev), "write simulated touch event");
    }

    void report(int t) { emit(t, EV_SYN, SYN_REPORT, 0); }

    void position(int t, int x, int y) {
        if (x >= 0) emit(t, EV_ABS, multitouch ? ABS_MT_POSITION_X : ABS_X, x);
        if (y >= 0) emit(t, EV_ABS, multitouch ? ABS_MT_POSITION_Y : ABS_Y, y);
    }

    void contact(int t, int slot, int id, int x = -1, int y = -1) {
        if (multitouch) {
            emit(t, EV_ABS, ABS_MT_SLOT, slot);
            emit(t, EV_ABS, ABS_MT_TRACKING_ID, id);
        } else {
            emit(t, EV_KEY, BTN_TOUCH, id >= 0);
        }
        position(t, x, y);
        report(t);
    }

    void double_tap(int t, int x, int y, int x2 = -1, int y2 = -1) {
        contact(t, 0, 10, x, y);
        contact(t + 60, 0, -1);
        // With unchanged coordinates, real evdev omits both position events.
        contact(t + 210, 0, 11, x2, y2);
        contact(t + 265, 0, -1);
    }

private:
    int output_fd_;
};

void verify_gestures() {
    EventPipe events;
    Viewport view(1024, 600, false);
    view.set_camera(640, 480);
    require(view.destination().x == 112 && view.destination().w == 800,
            "640x480 fits centered on the WaveShare panel");
    Viewport fill(1024, 600, true);
    fill.set_camera(640, 480);
    require(fill.source().x == 0 && fill.source().y == 53 && fill.source().h == 375,
            "fill uses a centered crop");

    TouchDevice touch;
    require(touch.open(path, "waveshare"), "open simulated MT-B touch device");
    events.double_tap(1000, 512, 300);
    auto update = touch.read_events(view, false);
    require(update.activity && update.changed && view.zoom() == 2.5, "stationary double tap zooms");
    require(view.source().x == 192 && view.source().y == 144, "zoom keeps tap point fixed");
    events.double_tap(1500, 512, 300, 516, 297);
    require(touch.read_events(view, false).changed && view.zoom() == 5, "tap jitter still zooms");
    events.double_tap(2000, 512, 300);
    require(touch.read_events(view, false).changed && view.zoom() == 1, "zoom cycle resets view");
    events.double_tap(2500, 200, 200, 800, 450);
    require(!touch.read_events(view, false).changed && view.zoom() == 1, "distant taps do not zoom");

    events.double_tap(3200, 512, 300);
    require(touch.read_events(view, false).changed, "zoom before drag test");
    const auto before = view.source();
    for (int i = 0; i < 6; ++i) {
        const int t = 3700 + i * 80;
        events.contact(t, 0, 20 + i, 500, 100 + i * 8);
        events.position(t + 20, 500, 120 + i * 8);
        events.report(t + 20);
        events.contact(t + 50, 0, -1);
    }
    update = touch.read_events(view, false);
    require(update.activity && !update.changed && view.zoom() == 2.5 &&
                view.source().x == before.x && view.source().y == before.y,
            "quick scroll flicks register as activity without moving the picture");
    events.contact(4500, 0, 26, 300, 300);
    events.contact(4510, 1, 27, 500, 300);
    events.position(4520, 600, 500);
    events.report(4520);
    events.contact(4550, 1, -1);
    events.emit(4560, EV_ABS, ABS_MT_SLOT, 0);
    events.position(4560, 400, 500);
    events.report(4560);
    events.contact(4570, 0, -1);
    require(!touch.read_events(view, false).changed && view.source().x == before.x &&
                view.source().y == before.y, "multitouch stays inert until both fingers lift");

    events.contact(5000, 0, 31, 1023, 599);
    events.position(5100, 0, 0);
    events.report(5100);
    events.contact(5110, 0, -1);
    require(!touch.read_events(view, false).changed && view.source().x == before.x &&
                view.source().y == before.y, "dragging across the display leaves the picture fixed");

    events.double_tap(6000, 512, 300);
    update = touch.read_events(view, true);
    require(update.activity && !update.changed && view.zoom() == 2.5, "wake touch is consumed");
    events.double_tap(6800, 512, 300);
    require(touch.read_events(view, false).changed && view.zoom() == 5, "gestures work after wake");

    events.contact(7700, 0, 44, 512, 300);
    events.contact(7760, 0, -1);
    events.emit(7800, EV_SYN, SYN_DROPPED, 0);
    events.report(7810);
    events.contact(7900, 0, 45, 512, 300);
    events.contact(7960, 0, -1);
    require(!touch.read_events(view, false).changed, "dropped sync clears pending tap");

    multitouch = false;
    require(touch.open(path, ""), "open simulated BTN_TOUCH device");
    events.double_tap(8500, 512, 300);
    require(touch.read_events(view, false).changed && view.zoom() == 1,
            "single touch devices support the same zoom gesture");

    const microcam::Rect button{896, 472, 128, 128};
    events.double_tap(9100, 980, 514);
    update = touch.read_events(view, false, button);
    require(update.screenshot && !update.changed && view.zoom() == 1,
            "tapping the screenshot button captures without zooming");
    events.contact(9800, 0, 1, 980, 550);
    events.contact(9860, 0, -1);
    update = touch.read_events(view, true, button);
    require(update.activity && !update.screenshot, "wake tap does not capture");

    const microcam::Rect reset{0, 472, 128, 128};
    events.double_tap(10300, 400, 300);
    require(touch.read_events(view, false, button, reset).changed && view.zoom() == 2.5,
            "double tap on the picture still zooms");
    events.contact(10900, 0, 1, 42, 550);
    events.contact(10960, 0, -1);
    update = touch.read_events(view, false, button, reset);
    require(update.changed && !update.screenshot && view.zoom() == 1 &&
                view.source().x == 0 && view.source().y == 0,
            "single tap on reset restores the full view");
    events.double_tap(11200, 42, 550);
    update = touch.read_events(view, false, button, reset);
    require(update.activity && !update.changed && !update.screenshot && view.zoom() == 1,
            "repeated reset taps do not zoom or capture");

    events.contact(12000, 0, 1, 600, 300);
    events.position(12040, 630, 300);
    events.report(12040);
    events.contact(12060, 0, -1);
    events.contact(12150, 0, 1, 978, 514);
    events.position(12200, 992, 514);
    events.report(12200);
    events.contact(12541, 0, -1);
    update = touch.read_events(view, false, button, reset);
    require(update.screenshot && !update.changed,
            "a slightly longer screenshot tap works after nearby movement");

    events.contact(13000, 0, 1, 500, 300);
    events.position(13100, 508, 300);
    events.report(13100);
    events.contact(13323, 0, -1);
    events.contact(13750, 0, 1, 510, 305);
    events.contact(13824, 0, -1);
    require(touch.read_events(view, false, button, reset).changed && view.zoom() == 2.5,
            "a slower pair with slight motion still zooms");
}

} // namespace

extern "C" int __wrap_open(const char* candidate, int, ...) { return mock_open(candidate); }
extern "C" int __wrap_open64(const char* candidate, int, ...) { return mock_open(candidate); }
extern "C" int __wrap___open_2(const char* candidate, int) { return mock_open(candidate); }
extern "C" int __wrap___open64_2(const char* candidate, int) { return mock_open(candidate); }

extern "C" int __wrap_ioctl(int, unsigned long request, ...) {
    va_list args;
    va_start(args, request);
    const int result = mock_ioctl(request, args);
    va_end(args);
    return result;
}

int main() {
    try {
        verify_gestures();
        std::puts("touch tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "touch test failed: %s\n", error.what());
        return 1;
    }
}
