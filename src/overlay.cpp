#include "overlay.h"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>   // openpty (BSD/macOS)
#else
#include <pty.h>    // openpty (glibc)
#endif

namespace overlay {

namespace {
// ark's own scrollback: the lines that have scrolled through the deadzone. A
// ring buffer (capped) that the mouse-wheel scroll view (step 2) renders from.
std::deque<std::string> g_scrollback;
std::string g_partial; // the in-progress last line (no newline yet)
constexpr size_t kMaxScrollback = 100000;

// Feed mirrored output bytes into the scrollback, splitting on '\n'. Carriage
// returns and escape sequences are kept verbatim for now (a later cooked pass
// can strip them for the scroll view); the point of this step is that NOTHING
// is lost when a line leaves the visible deadzone.
void capture(const char* buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            g_scrollback.push_back(g_partial);
            g_partial.clear();
            if (g_scrollback.size() > kMaxScrollback) g_scrollback.pop_front();
        } else if (c != '\r') {
            g_partial += c;
        }
    }
}

// Current real-terminal size (fallback 24x80).
void termSize(int& rows, int& cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    } else {
        rows = 24;
        cols = 80;
    }
}
} // namespace

bool enabled() {
    const char* v = getenv("ARK_OVERLAY");
    return v && std::string(v) == "1";
}

int run(const std::vector<std::string>& argv, char** env) {
    (void)env; // the child inherits ark's environ across fork(), like posix_spawnp
    if (argv.empty()) return -1;

    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) return -1;

    // Give the child PTY the real terminal's current size.
    int rows, cols;
    termSize(rows, cols);
    struct winsize ws{};
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ioctl(slave, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid < 0) { close(master); close(slave); return -1; }
    if (pid == 0) {
        // Child: new session, PTY slave becomes the controlling terminal.
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) close(slave);
        close(master);
        // Default signal dispositions -- ark ignores several for itself.
        for (int s : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU}) signal(s, SIG_DFL);
        std::vector<char*> c;
        for (auto& a : argv) c.push_back(const_cast<char*>(a.c_str()));
        c.push_back(nullptr);
        execvp(c[0], c.data());
        _exit(127);
    }

    close(slave);

    // Parent: put the REAL terminal in raw mode so keystrokes pass through to the
    // child byte-for-byte (the child's PTY runs its own line discipline).
    struct termios saved;
    bool haveSaved = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (haveSaved) {
        struct termios raw = saved;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    // Relay loop: real stdin -> child, child -> real stdout (+ scrollback).
    // Detect the child entering/leaving the alternate screen so a full-screen
    // TUI gets the whole terminal (drop our scroll region) and isn't captured.
    bool altScreen = false;
    char buf[8192];
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(master, &fds);
        int maxfd = master > STDIN_FILENO ? master : STDIN_FILENO;
        int rv = select(maxfd + 1, &fds, nullptr, nullptr, nullptr);
        if (rv < 0) { if (errno == EINTR) continue; break; }

        if (FD_ISSET(STDIN_FILENO, &fds)) {
            ssize_t k = read(STDIN_FILENO, buf, sizeof(buf));
            if (k > 0) { ssize_t off = 0; while (off < k) { ssize_t w = write(master, buf + off, k - off); if (w <= 0) break; off += w; } }
        }
        if (FD_ISSET(master, &fds)) {
            ssize_t k = read(master, buf, sizeof(buf));
            if (k <= 0) break; // child closed the PTY -> it's exiting
            // Track alt-screen: on enter, drop the scroll region so the TUI owns
            // the whole screen; on exit, the caller re-establishes chrome.
            std::string chunk(buf, (size_t)k);
            if (!altScreen && chunk.find("\x1b[?1049h") != std::string::npos) {
                altScreen = true;
                const char* reset = "\x1b[r";
                (void)!write(STDOUT_FILENO, reset, 3);
            }
            ssize_t off = 0;
            while (off < k) { ssize_t w = write(STDOUT_FILENO, buf + off, k - off); if (w <= 0) break; off += w; }
            if (!altScreen) capture(buf, (size_t)k); // only real scrollback, not TUI frames
            if (altScreen && chunk.find("\x1b[?1049l") != std::string::npos) altScreen = false;
        }
    }

    if (haveSaved) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    close(master);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

} // namespace overlay
