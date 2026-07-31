#include "vtmodel.h"

namespace scrollback {

int displayWidth(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x1b) {                    // ESC — skip an escape sequence
            i++;
            if (i < s.size() && s[i] == '[') {   // CSI: ESC [ ... <final 0x40..0x7e>
                i++;
                while (i < s.size() && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7e)) i++;
                if (i < s.size()) i++;            // consume final byte
            } else if (i < s.size()) {
                i++;                              // 2-byte escape
            }
            continue;
        }
        if (c >= 0x80 && c <= 0xbf) { i++; continue; }  // UTF-8 continuation: width 0
        w++;
        i++;
    }
    return w;
}

std::vector<std::string> wrapLine(const std::string& logical, int width) {
    if (width < 1) width = 1;
    std::vector<std::string> rows;
    std::string cur; int col = 0; std::string activeSgr;
    for (size_t i = 0; i < logical.size(); ) {
        unsigned char c = (unsigned char)logical[i];
        if (c == 0x1b) {                          // copy escape verbatim, track SGR
            std::string esc(1, (char)c); i++;
            if (i < logical.size() && logical[i] == '[') { esc += '['; i++;
                while (i < logical.size() && !((unsigned char)logical[i] >= 0x40 && (unsigned char)logical[i] <= 0x7e)) esc += logical[i++];
                if (i < logical.size()) { esc += logical[i]; i++; } }
            else if (i < logical.size()) { esc += logical[i]; i++; }
            cur += esc;
            if (esc.rfind("\x1b[", 0) == 0) activeSgr = esc;   // last SGR wins (best-effort)
            continue;
        }
        if (col >= width) { rows.push_back(cur); cur = activeSgr; col = 0; }
        cur += (char)c;
        if (!(c >= 0x80 && c <= 0xbf)) col++;      // continuation bytes are width 0
        i++;
    }
    rows.push_back(cur);
    return rows;
}

std::string VtModel::renderCells() const {
    std::string out;
    std::string lastSgr;
    for (const auto& c : cells_) {
        if (!c.sgr.empty() && c.sgr != lastSgr) { out += c.sgr; lastSgr = c.sgr; }
        out += c.set ? c.glyph : ' ';
    }
    // trim trailing unset/blank cells
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

void VtModel::flushLine() {
    completed_.push_back(LogicalLine{renderCells()});
    cells_.clear();
    pendingSgr_.clear();
    col_ = 0;
}

std::string VtModel::partial() const {
    return renderCells();
}

VtEvent VtModel::feed(const char* data, size_t n) {
    std::string chunk(data, n);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)data[i];
        if (ch == '\n') { flushLine(); continue; }
        if (ch == '\r') { col_ = 0; continue; }
        if (ch == '\b') { if (col_ > 0) col_--; continue; }
        if (ch == '\t') {
            int stop = (col_ / 8 + 1) * 8;
            while (col_ < stop) {
                if ((int)cells_.size() <= col_) cells_.resize(col_ + 1);
                cells_[col_].set = false;
                col_++;
            }
            continue;
        }
        if (ch == 0x1b) {                       // gather an escape sequence into pendingSgr_
            std::string esc(1, (char)ch);
            i++;
            if (i < n && data[i] == '[') {
                esc += '['; i++;
                while (i < n && !((unsigned char)data[i] >= 0x40 && (unsigned char)data[i] <= 0x7e)) esc += data[i++];
                if (i < n) esc += data[i];      // final byte; loop i++ advances past it
            } else if (i < n) {
                esc += data[i];
            }
            pendingSgr_ += esc;
            continue;
        }
        if ((int)cells_.size() <= col_) cells_.resize(col_ + 1);
        cells_[col_].glyph = (char)ch;
        cells_[col_].set = true;
        if (!pendingSgr_.empty()) { cells_[col_].sgr = pendingSgr_; pendingSgr_.clear(); }
        col_++;
    }

    VtEvent ev = VtEvent::None;
    auto scan = [&](const char* pat, VtEvent e) {
        size_t pos = 0;
        while ((pos = chunk.find(pat, pos)) != std::string::npos) { ev = e; pos++; }
    };
    scan("\x1b[?1049h", VtEvent::EnterAltScreen);
    scan("\x1b[?47h",   VtEvent::EnterAltScreen);
    scan("\x1b[?1049l", VtEvent::LeaveAltScreen);
    scan("\x1b[?47l",   VtEvent::LeaveAltScreen);
    return ev;
}

std::vector<LogicalLine> VtModel::takeCompleted() {
    std::vector<LogicalLine> out;
    out.swap(completed_);
    return out;
}

} // namespace scrollback
