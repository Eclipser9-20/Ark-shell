#include "chrome.h"
#include <cstdio>
#include <fstream>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

// Host-wide CPU utilization since the last call, from the delta between two
// host_statistics(HOST_CPU_LOAD_INFO) samples. Ticks are cumulative counters
// since boot, not an instantaneous rate, so this needs two points in time --
// hence the function-static previous sample. No prior sample yet (first call
// in the process) reports 0 rather than a bogus/huge percentage.
static double sampleCpuPercent() {
    static host_cpu_load_info_data_t prev{};
    static bool havePrev = false;

    host_cpu_load_info_data_t cur;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    kern_return_t kr = host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                                        (host_info_t)&cur, &count);
    if (kr != KERN_SUCCESS) return 0.0;

    double pct = 0.0;
    if (havePrev) {
        uint64_t userD = cur.cpu_ticks[CPU_STATE_USER] - prev.cpu_ticks[CPU_STATE_USER];
        uint64_t sysD = cur.cpu_ticks[CPU_STATE_SYSTEM] - prev.cpu_ticks[CPU_STATE_SYSTEM];
        uint64_t niceD = cur.cpu_ticks[CPU_STATE_NICE] - prev.cpu_ticks[CPU_STATE_NICE];
        uint64_t idleD = cur.cpu_ticks[CPU_STATE_IDLE] - prev.cpu_ticks[CPU_STATE_IDLE];
        uint64_t totalD = userD + sysD + niceD + idleD;
        if (totalD > 0) pct = 100.0 * (double)(userD + sysD + niceD) / (double)totalD;
    }
    prev = cur;
    havePrev = true;
    return pct;
}

HwStats getHwStats() {
    HwStats hw;

    double loadavg[3] = {0.0, 0.0, 0.0};
    if (getloadavg(loadavg, 3) != -1) {
        hw.load1 = loadavg[0];
    }

    hw.cpuPercent = sampleCpuPercent();

    uint64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0) {
        hw.memTotalGB = (double)memsize / (1024.0 * 1024.0 * 1024.0);
    }

    vm_statistics64_data_t vmstat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    kern_return_t kr = host_statistics64(mach_host_self(), HOST_VM_INFO64,
                                          (host_info64_t)&vmstat, &count);
    if (kr == KERN_SUCCESS) {
        uint64_t usedPages = vmstat.active_count + vmstat.wire_count + vmstat.compressor_page_count;
        double pageSize = (double)getpagesize();
        hw.memUsedGB = (usedPages * pageSize) / (1024.0 * 1024.0 * 1024.0);
    }

    return hw;
}

std::string findGitBranch(const std::string& cwd) {
    std::string dir = cwd;
    for (;;) {
        std::string gitDir = dir + "/.git";
        struct stat st;
        if (stat(gitDir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            std::ifstream headFile(gitDir + "/HEAD");
            std::string headLine;
            if (!headFile.is_open() || !std::getline(headFile, headLine)) return "";
            const std::string prefix = "ref: refs/heads/";
            if (headLine.rfind(prefix, 0) == 0) return headLine.substr(prefix.size());
            return headLine.size() >= 7 ? headLine.substr(0, 7) : headLine; // detached HEAD short SHA
        }
        if (dir == "/") return "";
        auto slash = dir.find_last_of('/');
        dir = (slash == std::string::npos || slash == 0) ? "/" : dir.substr(0, slash);
    }
}

static bool getTerminalSize(int& rows, int& cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0) {
        return false;
    }
    rows = ws.ws_row;
    cols = ws.ws_col;
    return true;
}

void setScrollRegion() {
    int rows, cols;
    if (!getTerminalSize(rows, cols) || rows <= 2) return;
    printf("\x1b[2;%dr", rows - 1);
    fflush(stdout);
}

static std::string formatSession(double seconds) {
    int total = (int)seconds;
    int h = total / 3600, m = (total % 3600) / 60, s = total % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%dh%02dm", h, m);
    else if (m > 0) snprintf(buf, sizeof(buf), "%dm%02ds", m, s);
    else snprintf(buf, sizeof(buf), "%ds", s);
    return buf;
}

namespace {

// TokyoNight Night palette -- reusing the exact colors already established
// in highlight.cpp's syntax highlighter (blue/purple/comment-gray), so the
// pinned bars visually match the rest of the shell instead of introducing a
// competing color scheme.
constexpr const char* FG_BLUE = "\x1b[38;2;122;162;247m";
constexpr const char* BG_BLUE = "\x1b[48;2;122;162;247m";
constexpr const char* FG_PURPLE = "\x1b[38;2;187;154;247m";
constexpr const char* BG_PURPLE = "\x1b[48;2;187;154;247m";
constexpr const char* FG_GRAY = "\x1b[38;2;86;95;137m";
constexpr const char* BG_GRAY = "\x1b[48;2;86;95;137m";
constexpr const char* FG_DARK = "\x1b[38;2;26;27;38m"; // TokyoNight bg color, used
                                                        // as the readable text
                                                        // color ON a colored chip
constexpr const char* RESET = "\x1b[0m";
constexpr const char* BG_DEFAULT = "\x1b[49m";

// FontAwesome-classic + Powerline codepoints -- the most universally-patched
// Nerd Font ranges (present even in minimal patched fonts), to minimize the
// risk of a missing glyph rendering as a tofu box.
constexpr const char* ICON_FOLDER = "\xef\x81\xbc"; // nf-fa-folder_open (U+F07C)
constexpr const char* ICON_BRANCH = "\xef\x84\xa6"; // nf-fa-code_fork, git-branch stand-in (U+F126)
constexpr const char* ICON_CLOCK = "\xef\x80\x97";  // nf-fa-clock_o (U+F017)
constexpr const char* ICON_USER = "\xef\x80\x87";   // nf-fa-user (U+F007)
constexpr const char* ICON_CPU = "\xef\x8b\x9b";     // nf-fa-microchip (U+F2DB)
constexpr const char* SEP_RIGHT = "\xee\x82\xb0";    // powerline solid right arrow (U+E0B0)
constexpr const char* SEP_LEFT = "\xee\x82\xb2";     // powerline solid left arrow, mirrored (U+E0B2)

// A colored "chip": bg/fg wrap `text` with one padding space on each side.
// `visibleWidth` (the on-screen column count of `text`) is tracked
// separately from ansi.size(), since `text` mixes 3-byte UTF-8 icon glyphs
// (1 display column each) with ASCII and ANSI color codes (0 display
// columns) -- byte length would badly overcount.
struct Chip {
    std::string ansi;
    int width;
};

Chip makeChip(const char* bg, const char* fg, const std::string& text, int visibleWidth) {
    return {std::string(bg) + fg + " " + text + " ", visibleWidth + 2};
}

} // namespace

void paintChrome(const std::string& cwd, const std::string& gitBranch,
                  double sessionSeconds, const HwStats& hw) {
    int rows, cols;
    if (!getTerminalSize(rows, cols) || rows <= 2) return;

    // ---- top bar: [folder + cwd] -> [branch icon + branch] -> default ----
    std::string dirText = cwd;
    int dirVisible = (int)cwd.size();
    int chipOverhead = 1 + 1 + 1 + 2; // icon + space-after-icon + leading/trailing chip spaces
    if (dirVisible + chipOverhead > cols) {
        int room = cols - chipOverhead;
        if (room < 0) room = 0;
        // Keep the tail of the path (e.g. ".../ark-terminal") -- the most
        // specific, most useful part when truncated -- rather than the head.
        dirText = (int)cwd.size() > room ? cwd.substr(cwd.size() - room) : cwd;
        dirVisible = (int)dirText.size();
    }
    Chip dirChip = makeChip(BG_BLUE, FG_DARK, std::string(ICON_FOLDER) + " " + dirText, dirVisible + 2);

    std::string topLine = dirChip.ansi;
    int topWidth = dirChip.width;
    bool showBranch = !gitBranch.empty() && topWidth + 1 + (int)gitBranch.size() + chipOverhead <= cols;
    if (showBranch) {
        topLine += FG_BLUE;
        topLine += BG_PURPLE;
        topLine += SEP_RIGHT; // arrow: blue segment exiting into purple
        Chip branchChip = makeChip(BG_PURPLE, FG_DARK, std::string(ICON_BRANCH) + " " + gitBranch,
                                    (int)gitBranch.size() + 2);
        topLine += branchChip.ansi;
        topLine += FG_PURPLE;
        topLine += BG_DEFAULT;
        topLine += SEP_RIGHT; // arrow: purple segment exiting into blank
    } else {
        topLine += FG_BLUE;
        topLine += BG_DEFAULT;
        topLine += SEP_RIGHT; // arrow: blue segment exiting into blank (no repo)
    }
    topLine += RESET;

    // ---- bottom bar: [user + session] ... padding ... [cpu + mem] ----
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    const char* user = getenv("USER");
    std::string userHost = std::string(user ? user : "user") + "@" + host;
    std::string sessionStr = formatSession(sessionSeconds);

    std::string leftText = std::string(ICON_USER) + " " + userHost + "  " + ICON_CLOCK + " " + sessionStr;
    int leftVisible = 1 + 1 + (int)userHost.size() + 2 + 1 + 1 + (int)sessionStr.size();
    Chip leftChip = makeChip(BG_GRAY, FG_DARK, leftText, leftVisible);

    char hwbuf[64];
    snprintf(hwbuf, sizeof(hwbuf), "%3.0f%%  mem %.1f/%.1fG", hw.cpuPercent, hw.memUsedGB, hw.memTotalGB);
    std::string hwStr = hwbuf;
    std::string rightText = std::string(ICON_CPU) + " " + hwStr;
    int rightVisible = 1 + 1 + (int)hwStr.size();
    Chip rightChip = makeChip(BG_BLUE, FG_DARK, rightText, rightVisible);

    int usedWidth = leftChip.width + 1 /* left arrow */ + 1 /* right arrow */ + rightChip.width;
    int pad = cols - usedWidth;
    if (pad < 1) pad = 1;

    std::string bottomLine = leftChip.ansi;
    bottomLine += FG_GRAY;
    bottomLine += BG_DEFAULT;
    bottomLine += SEP_RIGHT; // arrow: gray segment exiting into blank
    bottomLine += RESET;
    bottomLine += std::string(pad, ' ');
    bottomLine += FG_BLUE;
    bottomLine += BG_DEFAULT;
    bottomLine += SEP_LEFT; // arrow: blue segment entering from blank (right-aligned)
    bottomLine += rightChip.ansi;
    bottomLine += RESET;

    // NOTE: no save/restore-cursor here anymore -- see reassertChrome() for
    // why. This function only ever writes the two chrome rows; whoever
    // calls it is responsible for cursor safety around the call.
    printf("\x1b[1;1H\x1b[2K%s", topLine.c_str());
    printf("\x1b[%d;1H\x1b[2K%s", rows, bottomLine.c_str());
    fflush(stdout);
}

void reassertChrome(const std::string& cwd, const std::string& gitBranch,
                     double sessionSeconds, const HwStats& hw) {
    // Real bug found live: DECSTBM (sent by setScrollRegion()) has a
    // documented side effect -- it moves the cursor to absolute row 1, col 1
    // (since origin mode/DECOM is off by default). paintChrome() used to
    // save the cursor with DECSC *after* that jump already happened, so its
    // own DECRC restore just put the cursor back on row 1 every time --
    // exactly why typing appeared to land on top of the pinned top bar.
    // Also: DECSC/DECRC only hold ONE saved position (not a stack), so
    // nesting an outer save/restore around an inner one doesn't compose --
    // the inner save silently clobbers the outer one. Fixed by saving the
    // cursor ONCE, here, before setScrollRegion() runs, and restoring once
    // after paintChrome() -- paintChrome() itself no longer touches the
    // saved-cursor slot at all.
    printf("\x1b[?2026h");   // begin synchronized update
    printf("\x1b" "7");      // DECSC: save the TRUE cursor position, before
                              // DECSTBM's cursor-reset side effect
    setScrollRegion();
    paintChrome(cwd, gitBranch, sessionSeconds, hw);
    printf("\x1b" "8");      // DECRC: restore to the TRUE original position
    printf("\x1b[?2026l");   // end synchronized update
    fflush(stdout);
}
