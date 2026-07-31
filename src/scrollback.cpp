#include "scrollback.h"

namespace scrollback {

void Scrollback::setViewport(int width, int height) {
    width_ = width < 1 ? 1 : width;
    height_ = height < 1 ? 1 : height;
    scrollLines(0);   // re-clamp against the new geometry
}

std::vector<std::string> Scrollback::allPhysical() const {
    std::vector<std::string> phys;
    for (const auto& l : lines_) {
        auto rows = wrapLine(l.bytes, width_);
        for (auto& r : rows) phys.push_back(std::move(r));
    }
    return phys;
}

int Scrollback::maxOffset() const {
    int phys = (int)allPhysical().size();
    return phys > height_ ? phys - height_ : 0;
}

void Scrollback::push(const LogicalLine& l) {
    int added = (int)wrapLine(l.bytes, width_).size();
    lines_.push_back(l);
    while (lines_.size() > cap_) lines_.pop_front();
    if (offset_ > 0) offset_ += added;   // keep viewing the same rows as new output arrives
    scrollLines(0);                       // re-clamp to valid range
}

void Scrollback::scrollLines(int delta) {
    offset_ -= delta;                     // delta<0 (older) increases offset
    if (offset_ < 0) offset_ = 0;
    int mx = maxOffset();
    if (offset_ > mx) offset_ = mx;
}

std::vector<std::string> Scrollback::visibleRows() const {
    auto phys = allPhysical();
    int total = (int)phys.size();
    std::vector<std::string> out((size_t)height_, "");
    // When everything fits, top-align (content flows from the top like a real
    // terminal, blanks below) instead of pinning it to the bottom. When content
    // overflows, show a window whose bottom edge is `offset_` rows up from live.
    int top = (total <= height_) ? 0 : (total - height_ - offset_);
    for (int row = 0; row < height_; row++) {
        int idx = top + row;
        if (idx >= 0 && idx < total) out[(size_t)row] = phys[(size_t)idx];
    }
    return out;
}

} // namespace scrollback
