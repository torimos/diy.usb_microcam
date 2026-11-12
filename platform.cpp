#include "platform.hpp"

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <linux/gpio.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdexcept>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

namespace microcam {
namespace {

// The Linux text console is drawn into the framebuffer, the same memory the
// viewer writes. While it stays bound, getty, kernel messages, and the blinking
// cursor are painted straight over the picture, and the console blanker can
// black out the whole screen. Detach it for as long as the viewer runs.
constexpr const char* vtconsole_dir = "/sys/class/vtconsole";

std::string lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char ch : text) result += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return result;
}

bool read_text(const std::string& file, std::string& text, std::string& error) {
    const int fd = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = file + ": " + std::strerror(errno);
        return false;
    }
    char buffer[128]{};
    const auto length = ::read(fd, buffer, sizeof(buffer) - 1);
    const int code = errno;
    ::close(fd);
    if (length <= 0) {
        error = file + ": " + std::strerror(code ? code : EIO);
        return false;
    }
    text.assign(buffer, static_cast<std::size_t>(length));
    return true;
}

bool write_digit(const std::string& file, char digit, std::string& error) {
    const int fd = ::open(file.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        error = file + ": " + std::strerror(errno);
        return false;
    }
    const char text[] = {digit, '\n'};
    const auto written = ::write(fd, text, sizeof(text));
    const int code = errno;
    ::close(fd);
    if (written != static_cast<ssize_t>(sizeof(text))) {
        error = file + ": " + std::strerror(code ? code : EIO);
        return false;
    }
    return true;
}

// Collect the "bind" files of the consoles that draw on a framebuffer device.
std::vector<std::string> framebuffer_consoles(std::string& error) {
    std::vector<std::string> found;
    for (int index = 0;; ++index) {
        const auto directory = std::string(vtconsole_dir) + "/vtcon" + std::to_string(index);
        std::string name;
        if (!read_text(directory + "/name", name, error)) {
            if (index == 0) return found;  // No console class at all.
            error.clear();
            break;  // vtcon<index> is the first one that does not exist.
        }
        const auto label = lower(name);
        if (label.find("frame buffer") == std::string::npos && label.find("framebuffer") == std::string::npos)
            continue;
        std::string state, ignored;
        const auto bind = directory + "/bind";
        if (read_text(bind, state, ignored) && !state.empty() && state[0] == '1') found.push_back(bind);
    }
    return found;
}

}  // namespace

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
    detach_framebuffer_console();
}

void Console::detach_framebuffer_console() {
    std::string error;
    binds_ = framebuffer_consoles(error);
    if (!error.empty()) {
        warning_ = "could not check the text consoles: " + error;
        return;
    }
    for (const auto& bind : binds_) {
        if (write_digit(bind, '0', error)) continue;
        warning_ = "text console still on the screen: " + error;
        return;
    }
}

void Console::attach_framebuffer_console() {
    for (const auto& bind : binds_) {
        std::string ignored;
        write_digit(bind, '1', ignored);
    }
    binds_.clear();
}

Console::~Console() {
    if (fd_ >= 0) {
        ioctl(fd_, KDSETMODE, KD_TEXT);
        ::close(fd_);
    }
    attach_framebuffer_console();
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
