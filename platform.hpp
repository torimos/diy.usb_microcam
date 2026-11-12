#pragma once

#include <string>

namespace microcam {

class Console {
public:
    explicit Console(bool take_over);
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

private:
    int fd_ = -1;
};

class Backlight {
public:
    Backlight() = default;
    ~Backlight();
    Backlight(const Backlight&) = delete;
    Backlight& operator=(const Backlight&) = delete;

    bool open(const std::string& chip, unsigned line, std::string& error);
    bool set(bool on, std::string& error);

private:
    int write(bool on) const noexcept;
    int fd_ = -1;
    bool v2_ = false;
    bool on_ = true;
};

class StopSignal {
public:
    StopSignal();
    ~StopSignal();
    StopSignal(const StopSignal&) = delete;
    StopSignal& operator=(const StopSignal&) = delete;
    int fd() const { return fd_; }

private:
    int fd_;
};

}  // namespace microcam
