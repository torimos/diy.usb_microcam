#include "platform.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace microcam {

Console::Console(bool take_over) {
    if (!take_over) return;

    const int console = ::open("/dev/tty0", O_RDWR | O_CLOEXEC | O_NOCTTY);
    const char* tip = "; run with sudo on a text console (or use --no-vt for a dedicated framebuffer)";
    if (console < 0) {
        throw std::runtime_error(std::string("/dev/tty0: ") + std::strerror(errno) + tip);
    }
    // Resolve tty0 to a specific VT, so shutdown restores the same terminal.
    vt_stat info{};
    const int result = ioctl(console, VT_GETSTATE, &info);
    const int saved_errno = errno;
    ::close(console);
    if (result < 0) {
        throw std::runtime_error(std::string("VT_GETSTATE: ") + std::strerror(saved_errno) + tip);
    }
    const auto path = "/dev/tty" + std::to_string(info.v_active);
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd_ < 0) throw std::runtime_error(path + ": " + std::strerror(errno) + tip);

    int mode = 0;
    if (ioctl(fd_, KDGETMODE, &mode) < 0) {
        const int code = errno;
        ::close(fd_);
        throw std::runtime_error(path + " KDGETMODE: " + std::strerror(code) + tip);
    }
    if (mode != KD_TEXT) {
        ::close(fd_);
        throw std::runtime_error(path + " is already in graphics mode; stop the desktop/viewer or switch to a text console first");
    }
    if (ioctl(fd_, KDSETMODE, KD_GRAPHICS) < 0) {
        const int code = errno;
        ::close(fd_);
        throw std::runtime_error(path + " KDSETMODE: " + std::strerror(code) + tip);
    }
}

Console::~Console() {
    if (fd_ < 0) return;
    ioctl(fd_, KDSETMODE, KD_TEXT);
    ::close(fd_);
}

Backlight::~Backlight() {
    if (fd_ < 0) return;
    if (!on_) write(true);
    ::close(fd_);
}

bool Backlight::open(const std::string& chip, unsigned line, std::string& error) {
    const int fd = ::open(chip.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = chip + ": " + std::strerror(errno);
        return false;
    }

#ifdef GPIO_V2_GET_LINE_IOCTL
    gpio_v2_line_request request{};
    request.offsets[0] = line;
    request.num_lines = 1;
    std::strcpy(request.consumer, "usb-microcam");
    request.config.flags = GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_ACTIVE_LOW;
    request.config.num_attrs = 1;
    request.config.attrs[0].mask = 1;
    request.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    request.config.attrs[0].attr.values = 1;
    if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &request) == 0) {
        v2_ = true;
        fd_ = request.fd;
    }
    if (fd_ < 0 && errno != ENOTTY) {
        const int code = errno;
        ::close(fd);
        error = chip + " line " + std::to_string(line) + ": " + std::strerror(code);
        return false;
    }
#endif

    int code = 0;
    if (fd_ < 0) {
        gpiohandle_request legacy{};
        legacy.lineoffsets[0] = line;
        legacy.lines = 1;
        legacy.flags = GPIOHANDLE_REQUEST_OUTPUT | GPIOHANDLE_REQUEST_ACTIVE_LOW;
        legacy.default_values[0] = 1;
        std::strcpy(legacy.consumer_label, "usb-microcam");
        if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &legacy) == 0) fd_ = legacy.fd;
        else code = errno;
    }
    ::close(fd);
    if (fd_ >= 0) return true;
    error = chip + " line " + std::to_string(line) + ": " + std::strerror(code);
    return false;
}

int Backlight::write(bool on) const noexcept {
#ifdef GPIO_V2_LINE_SET_VALUES_IOCTL
    if (v2_) {
        gpio_v2_line_values values{};
        values.mask = 1;
        values.bits = on ? 1 : 0;
        return ioctl(fd_, GPIO_V2_LINE_SET_VALUES_IOCTL, &values);
    }
#endif
    gpiohandle_data values{};
    values.values[0] = on ? 1 : 0;
    return ioctl(fd_, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &values);
}

bool Backlight::set(bool on, std::string& error) {
    if (on == on_) return true;
    if (write(on) < 0) {
        error = std::strerror(errno);
        return false;
    }
    on_ = on;
    return true;
}

StopSignal::StopSignal() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigaddset(&mask, SIGQUIT);
    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        throw std::runtime_error(std::string("sigprocmask: ") + std::strerror(errno));
    }
    fd_ = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd_ < 0) throw std::runtime_error(std::string("signalfd: ") + std::strerror(errno));
}

StopSignal::~StopSignal() { ::close(fd_); }

}  // namespace microcam
