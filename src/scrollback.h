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
    int physicalRows() const { return (int)allPhysical().size(); }   // total wrapped rows
    int pendingBelow() const { return offset_; }   // physical rows below the window
    std::vector<std::string> visibleRows() const;  // exactly `height` rows
    size_t size() const { return lines_.size(); }
    // Monotonic count of every line ever pushed (survives eviction). The line
    // most recently pushed has sequence number pushed()-1.
    size_t pushed() const { return pushed_; }
    // Replace the still-resident line with absolute sequence `seq` (as returned
    // by pushed()-1 at push time). Returns false if that line has been evicted.
    bool replaceSeq(size_t seq, const LogicalLine& l);
    // Locate where logical line `seq` sits in the wrapped physical-row stream:
    // [firstRow, firstRow+count) indices into allPhysical() at the current width.
    // Lets a caller repaint JUST that line in place instead of the whole tail.
    // Returns false if `seq` has been evicted or never existed.
    bool physicalRangeOfSeq(size_t seq, int& firstRow, int& count) const;
private:
    size_t cap_;
    size_t pushed_ = 0;   // cumulative push count, including evicted lines
    std::deque<LogicalLine> lines_;
    int width_ = 80, height_ = 24;
    int offset_ = 0;                       // physical rows scrolled up from live bottom
    std::vector<std::string> allPhysical() const;
    int maxOffset() const;
};

} // namespace scrollback
