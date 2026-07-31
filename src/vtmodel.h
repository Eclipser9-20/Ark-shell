#pragma once
#include <string>
#include <vector>

namespace scrollback {

// One logical (unwrapped) line of terminal output. bytes include SGR escape
// sequences and the visible text, with no trailing newline.
struct LogicalLine { std::string bytes; };

// Alt-screen transition surfaced by VtModel::feed so the pager can hand the
// real screen to full-screen programs (vim/less/top) and stop capturing.
enum class VtEvent { None, EnterAltScreen, LeaveAltScreen };

// Visible column width of s, skipping ANSI escape sequences (CSI + 2-byte
// escapes). UTF-8 continuation bytes (0x80..0xBF) count as width 0 so a
// multibyte codepoint is best-effort width 1.
int displayWidth(const std::string& s);

// Split a logical line into physical rows of at most `width` visible columns,
// carrying the active SGR run across each wrap boundary. A zero-length logical
// line yields one empty row.
std::vector<std::string> wrapLine(const std::string& logical, int width);

// Converts a raw child byte stream into logical lines. Resolves in-line
// control bytes (CR/BS/TAB) against a per-line cell buffer so overwrites and
// tab stops land correctly while SGR color runs are preserved verbatim.
class VtModel {
public:
    explicit VtModel(int width) : width_(width < 1 ? 1 : width) {}
    VtEvent feed(const char* data, size_t n);
    std::vector<LogicalLine> takeCompleted();
    std::string partial() const;          // in-progress line as bytes
    int width() const { return width_; }
    void setWidth(int w) { width_ = (w < 1 ? 1 : w); }
private:
    struct Cell { std::string sgr; char glyph = ' '; bool set = false; };
    int width_;
    std::vector<Cell> cells_;
    std::string pendingSgr_;   // SGR bytes seen since last glyph, applied to next write
    int col_ = 0;
    std::vector<LogicalLine> completed_;
    void flushLine();
    std::string renderCells() const;
};

} // namespace scrollback
