#include "chrome.h"
#include "input.h"
#include "version.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/utsname.h> // uname() -- POSIX, used by the banner on every platform
#include <unistd.h>

// Platform layer: everything else in this file is POSIX, but hardware-stats
// and system-info gathering are inherently OS-specific. macOS uses Mach +
// sysctl; Linux reads /proc + sysinfo. Both compile from this one file.
#if defined(__APPLE__)
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

// Read a whole (small, /proc-style) file into a string. "" on failure.
[[maybe_unused]] static std::string readWholeFile(const char* path) {
    std::ifstream in(path);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Host-wide CPU utilization since the last call, as a delta between two
// cumulative-since-boot samples (so it needs two points in time -- hence the
// function-static previous sample; the first call reports 0). macOS reads Mach
// tick counters; Linux reads /proc/stat's aggregate "cpu" line.
static double sampleCpuPercent() {
#if defined(__APPLE__)
    static host_cpu_load_info_data_t prev{};
    static bool havePrev = false;
    host_cpu_load_info_data_t cur;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&cur, &count) != KERN_SUCCESS)
        return 0.0;
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
#elif defined(__linux__)
    // /proc/stat: "cpu  user nice system idle iowait irq softirq steal ...".
    static uint64_t prevIdle = 0, prevTotal = 0;
    static bool havePrev = false;
    std::string stat = readWholeFile("/proc/stat");
    if (stat.compare(0, 4, "cpu ") != 0 && stat.compare(0, 3, "cpu") != 0) return 0.0;
    std::istringstream is(stat.substr(0, stat.find('\n')));
    std::string label; is >> label;
    uint64_t v[8] = {0}, total = 0;
    int nread = 0;
    for (int i = 0; i < 8 && (is >> v[i]); i++) nread = i + 1;
    for (int i = 0; i < nread; i++) total += v[i];
    uint64_t idle = (nread > 3) ? v[3] + (nread > 4 ? v[4] : 0) : 0; // idle + iowait
    double pct = 0.0;
    if (havePrev && total > prevTotal) {
        uint64_t dTotal = total - prevTotal, dIdle = idle - prevIdle;
        pct = 100.0 * (double)(dTotal - dIdle) / (double)dTotal;
    }
    prevIdle = idle; prevTotal = total; havePrev = true;
    return pct;
#else
    return 0.0;
#endif
}

HwStats getHwStats() {
    HwStats hw;

    double loadavg[3] = {0.0, 0.0, 0.0};
    if (getloadavg(loadavg, 3) != -1) hw.load1 = loadavg[0]; // POSIX on both

    hw.cpuPercent = sampleCpuPercent();

#if defined(__APPLE__)
    uint64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0)
        hw.memTotalGB = (double)memsize / (1024.0 * 1024.0 * 1024.0);

    vm_statistics64_data_t vmstat;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vmstat, &count) == KERN_SUCCESS) {
        uint64_t usedPages = vmstat.active_count + vmstat.wire_count + vmstat.compressor_page_count;
        double pageSize = (double)getpagesize();
        hw.memUsedGB = (usedPages * pageSize) / (1024.0 * 1024.0 * 1024.0);
    }
#elif defined(__linux__)
    // /proc/meminfo gives MemTotal + MemAvailable (kB); used = total - avail,
    // which matches what `free -h` reports as "used+buffers/cache pressure".
    std::string mi = readWholeFile("/proc/meminfo");
    auto kb = [&](const char* key) -> double {
        size_t p = mi.find(key);
        if (p == std::string::npos) return 0;
        return strtod(mi.c_str() + p + strlen(key), nullptr); // value is in kB
    };
    double totalKb = kb("MemTotal:"), availKb = kb("MemAvailable:");
    hw.memTotalGB = totalKb / (1024.0 * 1024.0);
    hw.memUsedGB = (totalKb - availKb) / (1024.0 * 1024.0);
#endif

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

// Top-bar mode (ARK_CHROME_TOP):
//   "pinned" (DEFAULT) fixes the cwd+branch bar at row 1 (top-left, always
//     visible). Trades away native scrollback -- a scroll region that doesn't
//     start at row 1 can't feed the terminal's scrollback buffer.
//   "inline" prints the bar as a per-prompt header that scrolls with output, so
//     scrollback keeps working (main.cpp prints it; see topBar()).
//   "off" hides the top bar entirely (scrollback works; bottom bar only).
static bool chromeTopPinned() {
    const char* t = getenv("ARK_CHROME_TOP");
    if (!t) return true;                              // default: pinned
    std::string s = t;
    return s != "inline" && s != "off";              // anything unrecognized -> pinned
}
// The first scrollable row = the top margin of the scroll region. It's row 2
// only when a pinned top bar occupies row 1; otherwise row 1 is normal content.
static int chromeHomeRow() { return chromeTopPinned() ? 2 : 1; }

void setScrollRegion() {
    int rows, cols;
    if (!getTerminalSize(rows, cols) || rows <= 2) return;
    // Region = <top> .. N-1. Row N is always excluded so the pinned bottom bar
    // survives. <top> is row 1 by default -- lines only feed SCROLLBACK when
    // they scroll off row 1 -- or row 2 when a pinned top bar sits on row 1
    // (ARK_CHROME_TOP=pinned), which trades scrollback for the fixed header.
    printf("\x1b[%d;%dr", chromeHomeRow(), rows - 1);
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
constexpr const char* ICON_SSH = "\xef\x88\xb3";    // nf-fa-server (U+F233) -- remote/SSH session
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

// True when ark is running inside an SSH session: sshd exports SSH_CONNECTION /
// SSH_TTY / SSH_CLIENT into the login environment. When ark is the shell you
// land in over SSH, everything it shows (user@host, cwd, CPU/mem, clock) is
// ALREADY the remote machine's -- it reads the box it runs on. This just lets
// the chrome flag the session as remote so it's obvious at a glance. The SSH
// client's IP (for the banner) is the first field of SSH_CONNECTION.
static bool inSshSession() {
    return getenv("SSH_CONNECTION") || getenv("SSH_TTY") || getenv("SSH_CLIENT");
}
static std::string sshClientIp() {
    const char* c = getenv("SSH_CONNECTION"); // "<clientip> <cport> <serverip> <sport>"
    if (!c) return "";
    std::string s = c;
    size_t sp = s.find(' ');
    return sp == std::string::npos ? s : s.substr(0, sp);
}

// The top-bar content -- a rounded [dir pill][branch pill] -- as a STRING, not
// painted to a fixed row. It's printed inline as a header above each prompt (see
// main.cpp) so it scrolls with your output and lands in the terminal's
// scrollback, instead of being pinned to row 1. Pinning row 1 forced the scroll
// region to start at row 2, and terminals only feed scrollback from lines that
// scroll off row 1 -- so the old pinned top bar silently broke scroll-back
// entirely. Now only the BOTTOM stats bar is pinned (row N, outside a row-1..N-1
// region), which keeps scrollback working AND the live stats.
std::string topBar(const std::string& cwd, const std::string& gitBranch) {
    int rows, cols;
    if (!getTerminalSize(rows, cols)) cols = 80;
    int pillOverhead = 1 + 1 + 4; // icon + space-after-icon + 2 padding + 2 rounded caps
    std::string dirText = cwd;
    int dirVisible = (int)cwd.size();
    if (dirVisible + pillOverhead > cols) {
        int room = cols - pillOverhead;
        if (room < 0) room = 0;
        dirText = (int)cwd.size() > room ? cwd.substr(cwd.size() - room) : cwd; // keep the tail
        dirVisible = (int)dirText.size();
    }
    Chip dirPill = makePill(FG_BLUE, std::string(ICON_FOLDER) + " " + dirText, 1 + 1 + dirVisible);
    std::string line = dirPill.ansi;
    int width = dirPill.width;
    if (!gitBranch.empty() && width + (int)gitBranch.size() + pillOverhead <= cols) {
        Chip branchPill = makePill(FG_GREEN, std::string(ICON_BRANCH) + " " + gitBranch,
                                    1 + 1 + (int)gitBranch.size());
        line += branchPill.ansi; // no gap -- caps touch, forming the divider notch
    }
    return line;
}

void paintChrome(const std::string& cwd, const std::string& gitBranch,
                  double sessionSeconds, const HwStats& hw) {
    int rows, cols;
    if (!getTerminalSize(rows, cols) || rows <= 2) return;

    // ---- bottom bar: [user+session pill] ... padding ... [cpu+mem pill] ----
    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    const char* user = getenv("USER");
    std::string userHost = std::string(user ? user : "user") + "@" + host;
    std::string sessionStr = formatSession(sessionSeconds);

    // In an SSH session, swap the user icon for a "server" glyph and glow the
    // chip green -- an at-a-glance "you're on a remote box" signal. user@host
    // is already the REMOTE's (ark reads the machine it runs on).
    bool ssh = inSshSession();
    std::string leftText = std::string(ssh ? ICON_SSH : ICON_USER) + " " + userHost +
                           "  " + ICON_CLOCK + " " + sessionStr;
    int leftVisible = 1 + 1 + (int)userHost.size() + 2 + 1 + 1 + (int)sessionStr.size();
    Chip leftPill = makePill(ssh ? FG_GREEN : FG_INFO, leftText, leftVisible);

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

    // Pinned top bar (ARK_CHROME_TOP=pinned only): the cwd+branch header fixed
    // at row 1, which sits OUTSIDE the 2..N-1 scroll region so output never
    // disturbs it. In the default "inline" mode this is skipped -- the header is
    // printed by main.cpp above each prompt so it scrolls into scrollback.
    if (chromeTopPinned())
        printf("\x1b[1;1H\x1b[2K%s", topBar(cwd, gitBranch).c_str());

    // The pinned BOTTOM bar, absolute-positioned to the last row (outside the
    // scroll region, so ordinary output never disturbs it). Cursor safety around
    // this call is the caller's job (reassertChrome saves/restores).
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
#if defined(__APPLE__)
std::string sysctlStr(const char* name) {
    size_t len = 0;
    if (sysctlbyname(name, nullptr, &len, nullptr, 0) != 0 || len == 0) return "";
    std::string buf(len, '\0');
    if (sysctlbyname(name, buf.data(), &len, nullptr, 0) != 0) return "";
    while (!buf.empty() && buf.back() == '\0') buf.pop_back();
    return buf;
}
#endif

// Trim trailing whitespace/newlines (for /sys and /proc one-liners).
[[maybe_unused]] std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    return s;
}
#if defined(__linux__)
// Pull a value out of an /etc/os-release style KEY="value" / KEY=value line.
std::string osReleaseField(const std::string& text, const char* key) {
    size_t p = text.find(key);
    if (p == std::string::npos) return "";
    p += strlen(key);
    std::string v;
    for (; p < text.size() && text[p] != '\n'; p++) { if (text[p] == '"') continue; v += text[p]; }
    return v;
}
#endif

// ── Banner system-info (per-platform; each returns display text, "" unknown) ─
std::string biOsName() {           // "macOS 26.3.1" / "Ubuntu 22.04.3 LTS"
#if defined(__APPLE__)
    return "macOS " + sysctlStr("kern.osproductversion");
#elif defined(__linux__)
    std::string os = readWholeFile("/etc/os-release");
    std::string pretty = osReleaseField(os, "PRETTY_NAME=");
    if (!pretty.empty()) return pretty;
    std::string n = osReleaseField(os, "NAME="), v = osReleaseField(os, "VERSION_ID=");
    return n.empty() ? "Linux" : n + (v.empty() ? "" : " " + v);
#else
    return "";
#endif
}
std::string biKernel() {           // "Darwin 27.0.0" / "Linux 6.5.0" (uname is POSIX)
    struct utsname u;
    if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release;
    return "";
}
std::string biModel() {
#if defined(__APPLE__)
    return sysctlStr("hw.model");
#elif defined(__linux__)
    std::string m = rtrim(readWholeFile("/sys/devices/virtual/dmi/id/product_name"));
    if (!m.empty()) return m;
    struct utsname u; return uname(&u) == 0 ? u.machine : "";
#else
    return "";
#endif
}
std::string biCpu() {
#if defined(__APPLE__)
    std::string c = sysctlStr("machdep.cpu.brand_string");
    return c.empty() ? sysctlStr("hw.targettype") : c;
#elif defined(__linux__)
    std::string ci = readWholeFile("/proc/cpuinfo");
    size_t p = ci.find("model name");
    if (p == std::string::npos) p = ci.find("Model");           // ARM boards
    if (p == std::string::npos) return "";
    size_t colon = ci.find(':', p), nl = ci.find('\n', p);
    if (colon == std::string::npos || nl == std::string::npos || colon > nl) return "";
    return rtrim(ci.substr(colon + 1, nl - colon - 1)).erase(0, ci[colon + 1] == ' ' ? 1 : 0);
#else
    return "";
#endif
}
long biCores() { return sysconf(_SC_NPROCESSORS_ONLN); } // POSIX on both

std::string formatUptime() {
    long up = -1;
#if defined(__APPLE__)
    struct timeval bt {};
    size_t len = sizeof(bt);
    if (sysctlbyname("kern.boottime", &bt, &len, nullptr, 0) == 0 && bt.tv_sec != 0)
        up = (long)(time(nullptr) - bt.tv_sec);
#elif defined(__linux__)
    std::string u = readWholeFile("/proc/uptime");     // "<seconds>.<frac> <idle>"
    if (!u.empty()) up = (long)strtod(u.c_str(), nullptr);
#endif
    if (up < 0) return "";
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

    std::string osName = biOsName();   // full "macOS 26.3.1" / "Ubuntu 22.04"
    std::string kernel = biKernel();    // "Darwin 27.0.0" / "Linux 6.5.0"
    std::string model = biModel();
    std::string cpu = biCpu();
    long cores = biCores();
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

    // ── Logo (ARK_BANNER_LOGO = bolt | ark | none, default "bolt") ───────────
    // The default is a clean lightning bolt (ark's mark). "ark" is the block
    // wordmark (kept as an option), "none" shows no art.
    std::string logo = getenv("ARK_BANNER_LOGO") ? getenv("ARK_BANNER_LOGO") : "bolt";
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
        // Only non-empty in an SSH session, so it appears exactly when remote.
        {"ssh",    "SSH",    inSshSession() ? "remote session from " + sshClientIp() : ""},
        {"os",     "OS",     osName},
        {"kernel", "Kernel", kernel},
        {"shell",  "Shell",  "ark " ARK_VERSION},
        {"host",   "Host",   model},
        {"cpu",    "CPU",    cpuLine},
        {"mem",    "Mem",    memLine},
        {"uptime", "Uptime", up},
    };
    // Default omits "user" -- the bottom bar already shows user@host, so it'd be
    // redundant here. (ARK_BANNER_INFO can still ask for it explicitly.)
    std::string want = getenv("ARK_BANNER_INFO") ? getenv("ARK_BANNER_INFO")
                                                 : "ssh,os,kernel,shell,host,cpu,mem,uptime";
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
static bool queryCursorPos(int& outRow, int& outCol) {
    printf("\x1b[6n");
    fflush(stdout);

    // 60ms cap (was 200): a terminal answers DSR in well under 5ms, so a real
    // reply always arrives; if the terminal is momentarily busy or the reply is
    // lost we wait 60ms not 200 -- this runs after EVERY command, so a 200ms
    // stall there was a per-command "the shell feels slow." Overridable via
    // ARK_DSR_MS for slow remotes.
    long dsrMs = 60;
    if (const char* m = getenv("ARK_DSR_MS")) { long v = atol(m); if (v > 0) dsrMs = v; }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(dsrMs);

    // Strictly match the reply "ESC [ <digits> ; <digits> R". Any byte that
    // doesn't fit the grammar at its position is NOT part of the reply -- it's
    // the user's own input (a keystroke, or a paste that landed mid-query) -- so
    // it's handed back to the line editor via arkinput::enqueue() IN ORDER rather
    // than eaten. This is what stops a paste from losing its leading characters
    // and stops the reply from leaking onto the command line.
    enum State { WantEsc, WantBracket, WantRow, WantCol };
    State st = WantEsc;
    std::string pending, rowStr, colStr; // `pending` = bytes tentatively in the reply
    auto flushPending = [&]() { // reclassify the tentative reply bytes as user input
        if (!pending.empty()) arkinput::enqueue(pending.data(), pending.size());
        pending.clear(); rowStr.clear(); colStr.clear(); st = WantEsc;
    };

    for (;;) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            // Timed out. If we're mid-reply (pending looks like an "ESC[..."
            // prefix), it's almost certainly a slow/late DSR reply, NOT user
            // input -- DISCARD it so it never leaks onto the line. Only a stray
            // non-prefix byte would be worth handing back, and there is none here.
            return false;
        }
        char c;
        int rv = arkinput::readRawTimed(c, (int)remaining);
        if (rv == -2) return false; // timeout: discard any partial reply (see above)
        if (rv <= 0) { flushPending(); return false; } // EOF/error

        switch (st) {
            case WantEsc:
                if (c == '\x1b') { pending = c; st = WantBracket; }
                else arkinput::enqueue(&c, 1);           // plain user byte
                break;
            case WantBracket:
                if (c == '[') { pending += c; st = WantRow; }
                else { flushPending(); arkinput::enqueue(&c, 1); } // ESC then non-'[': user input
                break;
            case WantRow:
                if (isdigit((unsigned char)c)) { rowStr += c; pending += c; }
                else if (c == ';' && !rowStr.empty()) { pending += c; st = WantCol; }
                else { flushPending(); arkinput::enqueue(&c, 1); }
                break;
            case WantCol:
                if (isdigit((unsigned char)c)) { colStr += c; pending += c; }
                else if (c == 'R' && !colStr.empty()) {
                    outRow = std::atoi(rowStr.c_str());
                    outCol = std::atoi(colStr.c_str());
                    return true;
                }
                else { flushPending(); arkinput::enqueue(&c, 1); }
                break;
        }
    }
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
        printf("\x1b[%d;1H", chromeHomeRow()); // fresh cursor home (row 2 if a
                                               // pinned top bar owns row 1)
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
        printf("\x1b[%d;1H", chromeHomeRow()); // no valid prior position to
                                               // protect (startup); row 2 if a
                                               // pinned top bar owns row 1
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
        // The one terminal round-trip in the per-command path. It only powers
        // two corrections: (a) pulling the cursor off the pinned bottom bar
        // (rare -- the row-1..N-1 scroll region already keeps normal output out
        // of row N), and (b) the fresh-line when a command's output lacks a
        // trailing newline. Both are skipped when ARK_FRESHLINE=0, which also
        // skips the query entirely -- so a slow terminal can't make every
        // command pay a DSR stall.
        if (const char* f = getenv("ARK_FRESHLINE"); f && std::string(f) == "0") return;
        int rows, cols, row = -1, col = -1;
        bool got = getTerminalSize(rows, cols) && queryCursorPos(row, col);
        if (got && row >= rows) {
            printf("\x1b[%d;1H", rows - 1); // cursor on the bottom bar -> pull up one row
            fflush(stdout);
        } else if (got && row < chromeHomeRow()) {
            // `clear` homed the cursor to (1,1), which is ON the pinned top bar
            // (ARK_CHROME_TOP=pinned) -> push it down to the first scrollable row.
            printf("\x1b[%d;1H", chromeHomeRow());
            fflush(stdout);
        } else if (got && col > 1) {
            // Command output didn't end at column 1 (echo -n, a ^C mid-line) ->
            // one newline so the next prompt starts clean; nothing when it ended
            // cleanly, so a normal command never gains a spurious blank line.
            printf("\r\n");
            fflush(stdout);
        }
    }
}
