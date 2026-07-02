#include "chrome.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <vector>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <sys/ioctl.h>
#include <sys/select.h>
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

// "Dark neon sign" palette, per feedback: less pink (purple's gone
// entirely), rounded edges, blue/green/black/red kept dark rather than
// blindingly bright. All chips now share ONE near-black fill (TokyoNight's
// own bg_dark, #16161e -- a real, slightly-lighter-than-editor-bg tone
// TokyoNight itself defines for status-line-style UI chrome, not a made-up
// value) -- the "black" -- with each chip's icon+text glowing in a distinct
// accent color instead of the chip itself being a bright color block. Blue,
// green and red are the EXACT hexes already established elsewhere in this
// codebase (blue matches highlight.cpp's Command color; green/red match
// main.cpp's prompt success/fail arrow) rather than a new arbitrary "neon"
// palette, so the whole shell still reads as one consistent theme.
constexpr const char* BG_CHIP = "\x1b[48;2;22;22;30m";  // TokyoNight bg_dark (#16161e)
constexpr const char* FG_CHIP = "\x1b[38;2;22;22;30m";  // same color, as foreground -- used to
                                                         // draw the rounded caps below
constexpr const char* FG_BLUE = "\x1b[38;2;122;162;247m";
constexpr const char* FG_GREEN = "\x1b[38;2;158;206;106m";
constexpr const char* FG_RED = "\x1b[38;2;247;118;142m";
constexpr const char* FG_INFO = "\x1b[38;2;192;202;245m"; // TokyoNight normal fg -- neutral
                                                           // text for the user/session chip,
                                                           // which doesn't need to "glow"
constexpr const char* RESET = "\x1b[0m";
constexpr const char* BG_DEFAULT = "\x1b[49m";

// FontAwesome-classic + Powerline codepoints -- the most universally-patched
// Nerd Font ranges (present even in minimal patched fonts), to minimize the
// risk of a missing glyph rendering as a tofu box.
constexpr const char* ICON_FOLDER = "\xef\x81\xbc"; // nf-fa-folder_open (U+F07C)
constexpr const char* ICON_BRANCH = "\xef\x84\xa6"; // nf-fa-code_fork, git-branch stand-in (U+F126)
constexpr const char* ICON_CLOCK = "\xef\x80\x97";  // nf-fa-clock_o (U+F017)
constexpr const char* ICON_USER = "\xef\x80\x87";   // nf-fa-user (U+F007)
constexpr const char* ICON_CPU = "\xef\x83\xa7";    // nf-fa-bolt (U+F0E7)
// Rounded powerline dividers (as opposed to the hard-triangle E0B0/E0B2 used
// in the first pass) -- these render as a solid semicircle bump, giving each
// chip a pill/capsule shape instead of a sharp-cornered block.
constexpr const char* ROUND_LEFT_CAP = "\xee\x82\xb6";  // U+E0B6, opens a pill: "("
constexpr const char* ROUND_RIGHT_CAP = "\xee\x82\xb4"; // U+E0B4, closes a pill: ")"

// A rounded "pill": a BG_CHIP-filled capsule with a distinct fg accent color
// for its icon+text, opened/closed with the rounded caps above (drawn with
// fg=BG_CHIP's own color against the surrounding default background, which
// is what actually produces the rounded-bump shape). `visibleWidth` (the
// on-screen column count of `text`) is tracked separately from the ansi
// string's byte length, since `text` mixes 3-byte UTF-8 icon glyphs (1
// display column each) with ASCII and ANSI color codes (0 display columns).
struct Chip {
    std::string ansi;
    int width; // includes both rounded caps + the padding spaces inside them
};

Chip makePill(const char* accentFg, const std::string& text, int visibleWidth) {
    std::string s;
    s += BG_DEFAULT;
    s += FG_CHIP;
    s += ROUND_LEFT_CAP; // opening rounded cap, against the surrounding default bg
    s += BG_CHIP;
    s += accentFg;
    s += " " + text + " ";
    s += BG_DEFAULT;
    s += FG_CHIP;
    s += ROUND_RIGHT_CAP; // closing rounded cap
    s += RESET;
    return {s, visibleWidth + 4}; // +2 padding spaces, +2 rounded caps
}

} // namespace

void paintChrome(const std::string& cwd, const std::string& gitBranch,
                  double sessionSeconds, const HwStats& hw) {
    int rows, cols;
    if (!getTerminalSize(rows, cols) || rows <= 2) return;

    // ---- top bar: [dir pill][branch pill] ----
    // Adjacent pills now touch directly instead of leaving a plain-space gap
    // between them: with every pill sharing the same dark fill, a lone
    // rounded cap between two chips would be invisible (fg would equal bg,
    // no color boundary to reveal the bump) -- but placing one chip's
    // CLOSING cap immediately against the next chip's OPENING cap (no space
    // between them) still reads as a distinct rounded divider notch, the
    // classic lualine/powerline "connected rounded segments" look.
    int pillOverhead = 1 + 1 + 4; // icon + space-after-icon + 2 padding + 2 rounded caps
    std::string dirText = cwd;
    int dirVisible = (int)cwd.size();
    if (dirVisible + pillOverhead > cols) {
        int room = cols - pillOverhead;
        if (room < 0) room = 0;
        // Keep the tail of the path (e.g. ".../ark-terminal") -- the most
        // specific, most useful part when truncated -- rather than the head.
        dirText = (int)cwd.size() > room ? cwd.substr(cwd.size() - room) : cwd;
        dirVisible = (int)dirText.size();
    }
    Chip dirPill = makePill(FG_BLUE, std::string(ICON_FOLDER) + " " + dirText, 1 + 1 + dirVisible);

    std::string topLine = dirPill.ansi;
    int topWidth = dirPill.width;
    if (!gitBranch.empty() && topWidth + (int)gitBranch.size() + pillOverhead <= cols) {
        Chip branchPill = makePill(FG_GREEN, std::string(ICON_BRANCH) + " " + gitBranch,
                                    1 + 1 + (int)gitBranch.size());
        topLine += branchPill.ansi; // no gap -- caps touch, forming the divider notch
    }

    // ---- bottom bar: [user+session pill] ... padding ... [cpu+mem pill] ----
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    const char* user = getenv("USER");
    std::string userHost = std::string(user ? user : "user") + "@" + host;
    std::string sessionStr = formatSession(sessionSeconds);

    std::string leftText = std::string(ICON_USER) + " " + userHost + "  " + ICON_CLOCK + " " + sessionStr;
    int leftVisible = 1 + 1 + (int)userHost.size() + 2 + 1 + 1 + (int)sessionStr.size();
    Chip leftPill = makePill(FG_INFO, leftText, leftVisible);

    char hwbuf[64];
    snprintf(hwbuf, sizeof(hwbuf), "%3.0f%%  mem %.1f/%.1fG", hw.cpuPercent, hw.memUsedGB, hw.memTotalGB);
    std::string hwStr = hwbuf;
    // The bolt icon is colored blue specifically, then switches back to
    // FG_RED (makePill's base accent, applied before this text) for the
    // stats themselves -- a per-glyph color override embedded directly in
    // the text, since makePill only takes one accent color for the whole
    // chip otherwise.
    std::string rightText = std::string(FG_BLUE) + ICON_CPU + FG_RED + " " + hwStr;
    int rightVisible = 1 + 1 + (int)hwStr.size();
    Chip rightPill = makePill(FG_RED, rightText, rightVisible);

    int usedWidth = leftPill.width + rightPill.width;
    int pad = cols - usedWidth;
    if (pad < 1) pad = 1;

    std::string bottomLine = leftPill.ansi + std::string(pad, ' ') + rightPill.ansi;

    // NOTE: no save/restore-cursor here anymore -- see reassertChrome() for
    // why. This function only ever writes the two chrome rows; whoever
    // calls it is responsible for cursor safety around the call.
    printf("\x1b[1;1H\x1b[2K%s", topLine.c_str());
    printf("\x1b[%d;1H\x1b[2K%s", rows, bottomLine.c_str());
    fflush(stdout);
}

// ── Startup banner ("the zsh thing" + the ⚡ logo) ───────────────────────────
// A one-shot neofetch-style system panel printed once, right before the first
// prompt: an ASCII lightning bolt on the left (ark's mark) and everything we
// can cheaply learn about the machine on the right. All facts come from
// sysctl (no subprocess -- same latency discipline as getHwStats), so it adds
// no fork cost to startup. ARK_BANNER=0 turns it off from the config.
namespace {
std::string sysctlStr(const char* name) {
    size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return "";
    std::string buf(len, '\0');
    if (sysctlbyname(name, buf.data(), &len, nullptr, 0) != 0) return "";
    while (!buf.empty() && buf.back() == '\0') buf.pop_back();
    return buf;
}
long sysctlInt(const char* name) {
    int64_t v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0) return -1;
    return (long)v;
}
std::string formatUptime() {
    struct timeval bt {};
    size_t len = sizeof(bt);
    if (sysctlbyname("kern.boottime", &bt, &len, nullptr, 0) != 0 || bt.tv_sec == 0) return "";
    long up = (long)(time(nullptr) - bt.tv_sec);
    if (up < 0) up = 0;
    int d = up / 86400, h = (up % 86400) / 3600, m = (up % 3600) / 60;
    char buf[48];
    if (d > 0) snprintf(buf, sizeof(buf), "%dd %dh %dm", d, h, m);
    else if (h > 0) snprintf(buf, sizeof(buf), "%dh %dm", h, m);
    else snprintf(buf, sizeof(buf), "%dm", m);
    return buf;
}
} // namespace

// Resolve the banner accent color (ARK_BANNER_ACCENT): a named color or a raw
// 6-digit hex ("7aa2f7"). Falls back to ark's signature blue.
static std::string bannerAccent() {
    std::string a = getenv("ARK_BANNER_ACCENT") ? getenv("ARK_BANNER_ACCENT") : "";
    auto esc = [](int r, int g, int b) {
        char buf[32]; snprintf(buf, sizeof(buf), "\x1b[38;2;%d;%d;%dm", r, g, b); return std::string(buf);
    };
    if (a == "blue" || a.empty()) return esc(122, 162, 247);
    if (a == "green")  return esc(158, 206, 106);
    if (a == "red")    return esc(247, 118, 142);
    if (a == "purple") return esc(187, 154, 247);
    if (a == "pink")   return esc(255, 92, 205);
    if (a == "cyan")   return esc(125, 207, 255);
    if (a == "yellow") return esc(224, 175, 104);
    if (a == "orange") return esc(255, 158, 100);
    // Raw hex "rrggbb".
    if (a.size() == 6 && a.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
        int r = (int)strtol(a.substr(0, 2).c_str(), nullptr, 16);
        int g = (int)strtol(a.substr(2, 2).c_str(), nullptr, 16);
        int b = (int)strtol(a.substr(4, 2).c_str(), nullptr, 16);
        return esc(r, g, b);
    }
    return esc(122, 162, 247);
}

void printStartupBanner() {
    if (const char* b = getenv("ARK_BANNER"); b && std::string(b) == "0") return;

    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    const char* user = getenv("USER");
    std::string userHost = std::string(user ? user : "user") + "@" + host;

    std::string osVer = sysctlStr("kern.osproductversion");
    std::string kernel = sysctlStr("kern.osrelease");
    std::string model = sysctlStr("hw.model");
    std::string cpu = sysctlStr("machdep.cpu.brand_string");
    if (cpu.empty()) cpu = sysctlStr("hw.targettype");
    long cores = sysctlInt("hw.logicalcpu");
    HwStats hw = getHwStats();
    std::string up = formatUptime();

    char cpuLine[128];
    if (cores > 0) snprintf(cpuLine, sizeof(cpuLine), "%s (%ld)", cpu.c_str(), cores);
    else snprintf(cpuLine, sizeof(cpuLine), "%s", cpu.c_str());
    char memLine[96];
    snprintf(memLine, sizeof(memLine), "%.1f / %.1f GiB", hw.memUsedGB, hw.memTotalGB);

    std::string A = bannerAccent();               // logo accent (configurable)
    const char* G = FG_GREEN;
    const char* I = FG_INFO;
    const char* D = "\x1b[38;2;86;95;137m";       // comment gray (labels / subtitle)
    const char* R = RESET;

    // ── Logo (ARK_BANNER_LOGO = ark | bolt | none, default "ark") ────────────
    // "ark" is a smooth rounded wordmark drawn with quadrant blocks (the same
    // family the pinned-bar pills use), giving the soft LazyVim-dashboard feel.
    std::string logo = getenv("ARK_BANNER_LOGO") ? getenv("ARK_BANNER_LOGO") : "ark";
    std::vector<std::string> art;
    if (logo == "ark") {
        // Block letters with ROUNDED corners (╭╮╰╯) -- legible and soft, the
        // LazyVim-dashboard look.
        art = {
            " █████╮ ██████╮ ██╮  ██╮",
            "██╭──██╮██╭──██╮██║ ██╭╯",
            "███████║██████╭╯█████╭╯ ",
            "██╭──██║██╭──██╮██╭─██╮ ",
            "██║  ██║██║  ██║██║  ██╮",
            "╰─╯  ╰─╯╰─╯  ╰─╯╰─╯  ╰─╯",
        };
    } else if (logo == "bolt") {
        art = {
            "    ▟█▛ ", "   ▟█▛  ", "  ▟███▙ ", "  ▀▜██▛ ",
            "    ▟█▛ ", "   ▟█▛  ", "  ▟▛    ",
        };
    } // "none" -> no art

    // Subtitle (ARK_BANNER_SUBTITLE overrides the default tagline).
    std::string subtitle = getenv("ARK_BANNER_SUBTITLE") ? getenv("ARK_BANNER_SUBTITLE")
                                                         : "the best of all worlds";

    printf("\r\n");
    for (const auto& line : art) printf("  %s%s%s\r\n", A.c_str(), line.c_str(), R);
    if (!art.empty()) {
        // The ⚡ + subtitle sit just under the wordmark.
        printf("  %s%s%s %s%s%s\r\n\r\n", A.c_str(), ICON_CPU, R, D, subtitle.c_str(), R);
    }

    // ── Info fields (ARK_BANNER_INFO = comma list; default = all, in order) ──
    // Field keys: user, os, kernel, shell, host, cpu, mem, uptime.
    struct Field { const char* key; std::string label; std::string value; };
    std::vector<Field> all = {
        {"user",   "",       userHost},
        {"os",     "OS",     "macOS " + osVer},
        {"kernel", "Kernel", "Darwin " + kernel},
        {"shell",  "Shell",  "ark 1.0.0"},
        {"host",   "Host",   model},
        {"cpu",    "CPU",    cpuLine},
        {"mem",    "Mem",    memLine},
        {"uptime", "Uptime", up},
    };
    std::string want = getenv("ARK_BANNER_INFO") ? getenv("ARK_BANNER_INFO")
                                                 : "user,os,kernel,shell,host,cpu,mem,uptime";
    auto wants = [&](const char* key) {
        if (want == "all") return true;
        std::string k = key;
        size_t pos = 0;
        while (pos <= want.size()) {
            size_t comma = want.find(',', pos);
            std::string seg = comma == std::string::npos ? want.substr(pos) : want.substr(pos, comma - pos);
            // trim spaces
            size_t a = seg.find_first_not_of(' '), b = seg.find_last_not_of(' ');
            if (a != std::string::npos) seg = seg.substr(a, b - a + 1);
            if (seg == k) return true;
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return false;
    };
    for (const auto& f : all) {
        if (!wants(f.key) || f.value.empty()) continue;
        if (f.label.empty()) printf("  %s%s%s\r\n", G, f.value.c_str(), R);           // user@host
        else printf("  %s%-7s%s %s%s%s\r\n", D, f.label.c_str(), R, I, f.value.c_str(), R);
    }
    printf("\r\n");
    fflush(stdout);
}

// Queries the terminal's actual cursor row via DSR (\x1b[6n), reading the
// \x1b[<row>;<col>R response synchronously from stdin (only safe to call
// while stdin is already in raw mode -- see CursorPolicy::VerifyAndCorrect's
// caller, which holds a RawMode guard for the whole reassert). Returns -1 on
// any failure (terminal doesn't support DSR, response is malformed, or
// nothing arrives within ~200ms) -- callers should treat -1 as "unknown,
// leave the cursor alone" rather than guessing.
//
// This does read real bytes off stdin, so it's not entirely free of risk: a
// keystroke the user types in the same instant could in principle interleave
// with the response. In practice this window is a handful of milliseconds
// (terminals answer DSR essentially instantly, far faster than a human can
// type a next keystroke) -- the same tradeoff tools like fzf/tmux/zsh prompt
// frameworks already make when they query cursor position this way.
static int queryCursorRow() {
    printf("\x1b[6n");
    fflush(stdout);

    std::string resp;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    for (;;) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::milliseconds(0)) return -1;
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();
        struct timeval tv;
        tv.tv_sec = (long)(us / 1000000);
        tv.tv_usec = (long)(us % 1000000);

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int rv = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if (rv <= 0) return -1; // timeout, or a signal interrupted the wait
                                 // (rare here, and not worth retrying for a
                                 // best-effort correction) -- caller falls
                                 // back to leaving the cursor alone

        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) return -1;
        resp += c;
        if (c == 'R') break;
        if (resp.size() > 32) return -1; // sanity guard against runaway input
    }

    size_t esc = resp.find("\x1b[");
    size_t semi = esc == std::string::npos ? std::string::npos : resp.find(';', esc);
    if (esc == std::string::npos || semi == std::string::npos) return -1;
    std::string rowStr = resp.substr(esc + 2, semi - (esc + 2));
    if (rowStr.empty()) return -1;
    for (char ch : rowStr) {
        if (!isdigit((unsigned char)ch)) return -1;
    }
    return std::atoi(rowStr.c_str());
}

// Set once by reassertChrome() when it does a resize-driven full repaint;
// consumed (and cleared) by chromeConsumeResizeRepaint(). See the header.
static bool g_didResizeRepaint = false;
bool chromeConsumeResizeRepaint() {
    bool v = g_didResizeRepaint;
    g_didResizeRepaint = false;
    return v;
}

void reassertChrome(const std::string& cwd, const std::string& gitBranch,
                     double sessionSeconds, const HwStats& hw, CursorPolicy policy) {
    // Config toggle: ARK_CHROME=0 disables the pinned top/bottom bars. When
    // off, reset the scroll region to the full screen (\x1b[r) so the whole
    // terminal scrolls normally, and paint nothing. Cheap to check each call.
    if (const char* c = getenv("ARK_CHROME"); c && std::string(c) == "0") {
        printf("\x1b[r");
        fflush(stdout);
        return;
    }

    // Resize handling. A terminal resize REFLOWS the screen -- the terminal
    // moves the previously-visible content (including our pinned top/bottom
    // bars) to new rows, and WHERE they land depends on scrollback state we
    // can't observe: grow-with-scrollback shifts everything down and reveals
    // history at the top (stranding an old bar mid-screen), grow-without
    // pins content to the top and adds blank rows at the bottom (stranding a
    // DIFFERENT row). No fixed-row erase can chase a ghost the terminal
    // itself relocated -- which is exactly why the earlier single-row cleanup
    // fixed one resize direction but not the other. So on ANY geometry change
    // we do the standard SIGWINCH discipline every full-screen TUI uses:
    // clear the visible screen and repaint from scratch. \x1b[2J erases only
    // the display, never the scrollback, so scroll-up history is preserved;
    // the caller (readLine, via chromeConsumeResizeRepaint()) reprints its
    // prompt line afterward since the clear wiped it.
    int rows, cols;
    bool haveSize = getTerminalSize(rows, cols) && rows > 2;
    static int lastRows = 0, lastCols = 0;
    bool resized = haveSize && lastRows != 0 && (rows != lastRows || cols != lastCols);
    if (haveSize) { lastRows = rows; lastCols = cols; }

    if (resized) {
        printf("\x1b[?2026h"); // begin synchronized update (no flicker)
        printf("\x1b[r");      // drop the scroll region so 2J clears everything
        printf("\x1b[2J");     // erase the whole visible screen -- ghosts and all
        setScrollRegion();     // re-establish the top/bottom margins
        paintChrome(cwd, gitBranch, sessionSeconds, hw);
        printf("\x1b[2;1H");   // fresh cursor home inside the scroll region
        printf("\x1b[?2026l"); // end synchronized update
        fflush(stdout);
        g_didResizeRepaint = true;
        return;                // skip DECSC/DECRC and VerifyAndCorrect -- there is
                               // no meaningful prior cursor position to restore
                               // after a full clear
    }

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
    printf("\x1b[?2026h"); // begin synchronized update
    if (policy != CursorPolicy::ForceReseed) {
        printf("\x1b" "7"); // DECSC: save the TRUE cursor position, before
                             // DECSTBM's cursor-reset side effect
    }
    setScrollRegion();
    paintChrome(cwd, gitBranch, sessionSeconds, hw);
    if (policy == CursorPolicy::ForceReseed) {
        printf("\x1b[2;1H"); // no valid prior position to protect (startup)
    } else {
        printf("\x1b" "8"); // DECRC: restore to the TRUE original position
    }
    printf("\x1b[?2026l"); // end synchronized update
    fflush(stdout);

    if (policy == CursorPolicy::VerifyAndCorrect) {
        // A second bug found live: unconditionally forcing the cursor to
        // row 2 after EVERY command (this policy's first attempt) fixed
        // `clear` but broke the common case -- a normal multi-line
        // command's output should let the next prompt continue right where
        // it left off, not get yanked back to the top every time (looked
        // like text printing upward instead of down). The DECRC above
        // already restores to wherever the command actually left the
        // cursor, which is correct for ordinary commands; only verify and
        // correct if that turns out to be genuinely invalid (`clear`-family
        // commands home the cursor to (1,1) via their own \x1b[H).
        int rows, cols;
        int row = getTerminalSize(rows, cols) ? queryCursorRow() : -1;
        if (row != -1 && (row <= 1 || row >= rows)) {
            printf("\x1b[2;1H");
            fflush(stdout);
        }
    }
}
