#pragma once

#include <string>
#include <vector>

namespace microcam {

// Owns the virtual console while the viewer runs: the VT goes to graphics mode
// and the text console is detached from the framebuffer, so nothing else can
// draw on the screen the viewer owns. Restored on destruction.
class Console {
public:
    explicit Console(bool take_over);
    ~Console();
    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // Set when the text console stayed attached to the framebuffer. The viewer
    // still works, but login prompts or kernel messages can overlap the picture.
    const std::string& warning() const noexcept { return warning_; }

private:
    void detach_framebuffer_console();
    void attach_framebuffer_console();

    int fd_ = -1;
    std::vector<std::string> binds_;
    std::string warning_;
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
