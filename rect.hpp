#pragma once

namespace microcam {

struct Rect {
    int x, y, w, h;

    bool contains(double px, double py) const noexcept {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

} // namespace microcam
