#pragma once
#include "vtmodel.h"
#include <deque>
#include <string>
#include <vector>

namespace scrollback {

// A capped ring of logical lines plus viewport math. Lines are stored
// unwrapped; wrapping to the current width happens at paint time, so a
// terminal resize reflows for free. The viewport is a window of `height`
// physical rows into the wrapped content at the current scroll offset.
class Scrollback {
public:
    explicit Scrollback(size_t cap) : cap_(cap < 1 ? 1 : cap) {}
    void push(const LogicalLine& l);
    void setViewport(int width, int height);
    void scrollLines(int delta);          // delta<0 => older, delta>0 => newer
    void snapToLive() { offset_ = 0; }
    bool atLive() const { return offset_ == 0; }
    bool canScroll() const { return maxOffset() > 0; }   // content overflows the region
    int pendingBelow() const { return offset_; }   // physical rows below the window
    std::vector<std::string> visibleRows() const;  // exactly `height` rows
    size_t size() const { return lines_.size(); }
private:
    size_t cap_;
    std::deque<LogicalLine> lines_;
    int width_ = 80, height_ = 24;
    int offset_ = 0;                       // physical rows scrolled up from live bottom
    std::vector<std::string> allPhysical() const;
    int maxOffset() const;
};

} // namespace scrollback
