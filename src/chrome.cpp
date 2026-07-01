#include "chrome.h"
#include <cstdio>
#include <fstream>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/vm_statistics.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

HwStats getHwStats() {
    HwStats hw;

    double loadavg[3] = {0.0, 0.0, 0.0};
    if (getloadavg(loadavg, 3) != -1) {
        hw.load1 = loadavg[0];
    }

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

void paintChrome(const std::string& cwd, const std::string& gitBranch,
                  double sessionSeconds, const HwStats& hw) {
    int rows, cols;
    if (!getTerminalSize(rows, cols) || rows <= 2) return;

    std::string top = cwd;
    if (!gitBranch.empty()) top += "  " + gitBranch;
    if ((int)top.size() > cols) top = top.substr(0, cols);

    char host[256] = {0};
    gethostname(host, sizeof(host) - 1);
    const char* user = getenv("USER");
    std::string left = std::string(user ? user : "user") + "@" + host + " " + formatSession(sessionSeconds);

    char hwbuf[128];
    snprintf(hwbuf, sizeof(hwbuf), "load %.2f  mem %.1f/%.1fG", hw.load1, hw.memUsedGB, hw.memTotalGB);
    std::string right = hwbuf;

    std::string bottom = left;
    int pad = cols - (int)left.size() - (int)right.size();
    if (pad < 1) pad = 1;
    bottom += std::string(pad, ' ') + right;
    if ((int)bottom.size() > cols) bottom = bottom.substr(0, cols);

    printf("\x1b[?2026h");    // begin synchronized update
    printf("\x1b" "7");       // save cursor (DECSC) -- split literal: "\x1b7"
                               // would parse as the hex escape "\x1b7" (a
                               // 3-digit hex value, out of range), since \x
                               // greedily consumes hex digits and '7' is one
    printf("\x1b[1;1H\x1b[2K%s", top.c_str());
    printf("\x1b[%d;1H\x1b[2K%s", rows, bottom.c_str());
    printf("\x1b" "8");       // restore cursor (DECRC), same split-literal fix
    printf("\x1b[?2026l");    // end synchronized update
    fflush(stdout);
}

void reassertChrome(const std::string& cwd, const std::string& gitBranch,
                     double sessionSeconds, const HwStats& hw) {
    setScrollRegion();
    paintChrome(cwd, gitBranch, sessionSeconds, hw);
}
