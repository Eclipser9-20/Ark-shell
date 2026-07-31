#include "nucapture.h"
#include "value.h"
#include "jobs.h"   // BlockSigchld -- the global SIGCHLD handler must not reap our child
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <set>
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

namespace nucapture {
namespace {

void termSize(int& rows, int& cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) { rows = ws.ws_row; cols = ws.ws_col; }
    else { rows = 24; cols = 80; }
}

void writeAll(int fd, const char* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; break; }
        off += (size_t)w;
    }
}

// First non-whitespace byte of s, or 0 if s is empty/all whitespace so far.
char firstNonWs(const std::string& s) {
    for (char c : s) { if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue; return c; }
    return 0;
}

} // namespace

bool enabled() {
    const char* v = getenv("ARK_NU_MODE");
    return v && std::string(v) == "1";
}

bool isInteractiveCommand(const std::string& name) {
    std::string b = name;
    auto sl = b.find_last_of('/');
    if (sl != std::string::npos) b = b.substr(sl + 1);
    // Editors, pagers, full-screen TUIs, REPLs, long-lived streamers -- anything
    // that owns the terminal or runs indefinitely. These keep the plain spawn
    // path (real tty + working Ctrl-Z), and would never emit a JSON doc anyway.
    static const std::set<std::string> kInteractive = {
        "vim","vi","nvim","view","nano","pico","emacs","ne","micro","kak","helix","hx",
        "less","more","most","man","info","bat",
        "top","htop","btop","btop++","glances","watch","tail","journalctl",
        "ssh","tmux","screen","mosh","telnet","nc","ncat",
        "lazygit","tig","gitui","ranger","vifm","nnn","lf","ncdu","fzf","sk","yazi",
        "mutt","neomutt","w3m","lynx","links","irssi","weechat","newsboat",
        "ping","python","python3","node","irb","ipython","psql","mysql","sqlite3",
        "redis-cli","mongo","bc","gdb","lldb","R","julia","ghci","iex","clj",
    };
    return kInteractive.count(b) > 0;
}

int run(const std::vector<std::string>& argv, char** env) {
    (void)env; // the child inherits ark's environ across fork(), like posix_spawnp
    if (argv.empty()) return -1;

    int master = -1, slave = -1;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) != 0) return -1;

    int rows, cols;
    termSize(rows, cols);
    struct winsize ws{};
    ws.ws_row = (unsigned short)rows;
    ws.ws_col = (unsigned short)cols;
    ioctl(slave, TIOCSWINSZ, &ws);

    // The global SIGCHLD handler reaps ANY child (waitpid(-1, WNOHANG|...)). Without
    // this guard it reaped our PTY child before the waitpid() below, which then
    // returned ECHILD leaving status == 0 -- so under ARK_NU_MODE every command
    // reported SUCCESS: `false` looked true, and every `&&` / `if cmd` broke.
    // Every other foreground wait path in ark takes this same guard.
    BlockSigchld sigchldGuard;

    pid_t pid = fork();
    if (pid < 0) { close(master); close(slave); return -1; }
    if (pid == 0) {
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) close(slave);
        close(master);
        for (int s : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU}) signal(s, SIG_DFL);
        std::vector<char*> c;
        for (auto& a : argv) c.push_back(const_cast<char*>(a.c_str()));
        c.push_back(nullptr);
        execvp(c[0], c.data());
        _exit(127);
    }

    close(slave);

    // Real terminal -> raw, so keystrokes pass through to the child unchanged
    // (the child's PTY runs its own line discipline).
    struct termios saved;
    bool haveSaved = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (haveSaved) {
        struct termios raw = saved;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    // Output policy: DECIDING until we see the first non-whitespace byte; if it
    // is '{' or '[' we buffer the whole stream as a JSON candidate, otherwise we
    // flush what we have and stream the rest verbatim (PASS). An alt-screen TUI
    // is detected and switched to PASS (with the scroll region dropped).
    enum { DECIDING, JSON, PASS } mode = DECIDING;
    std::string buf;
    bool altSeen = false;
    const size_t kCap = 16u * 1024u * 1024u; // give up buffering past this
    char rb[65536];

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        FD_SET(master, &fds);
        int maxfd = master > STDIN_FILENO ? master : STDIN_FILENO;
        struct timeval tv{0, 200000}; // 200ms: poll child state so a stop can't hang us
        int rv = select(maxfd + 1, &fds, nullptr, nullptr, &tv);
        if (rv < 0) { if (errno == EINTR) continue; break; }
        if (rv == 0) {
            // Ctrl-Z relayed to the child stops it; we can't suspend a captured
            // command, so resume it rather than deadlock the select loop.
            int st;
            pid_t r = waitpid(pid, &st, WNOHANG | WUNTRACED);
            if (r == pid && WIFSTOPPED(st)) kill(pid, SIGCONT);
            continue;
        }
        if (FD_ISSET(STDIN_FILENO, &fds)) {
            ssize_t k = read(STDIN_FILENO, rb, sizeof rb);
            if (k > 0) writeAll(master, rb, (size_t)k);
        }
        if (FD_ISSET(master, &fds)) {
            ssize_t k = read(master, rb, sizeof rb);
            // ark's 1Hz idle ticker (SIGALRM, installed without SA_RESTART) and
            // SIGWINCH both interrupt this read. Treating EINTR as EOF abandoned a
            // LIVE child: we'd close(master), then block forever in waitpid() while
            // the child blocked writing to a PTY nobody was reading -- an unkillable
            // hung shell. Only a real 0/error is end-of-child.
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break; // child closed the PTY -> it's exiting
            if (mode == PASS) { writeAll(STDOUT_FILENO, rb, (size_t)k); continue; }

            std::string chunk(rb, (size_t)k);
            if (!altSeen && chunk.find("\x1b[?1049h") != std::string::npos) {
                // Full-screen TUI: give it the whole screen and stop capturing.
                altSeen = true;
                const char* reset = "\x1b[r";
                writeAll(STDOUT_FILENO, reset, 3);
                if (!buf.empty()) writeAll(STDOUT_FILENO, buf.data(), buf.size());
                writeAll(STDOUT_FILENO, rb, (size_t)k);
                buf.clear();
                mode = PASS;
                continue;
            }
            buf.append(rb, (size_t)k);
            if (mode == DECIDING) {
                char c = firstNonWs(buf);
                if (c == 0 && buf.size() < 65536) continue; // only whitespace so far -- wait
                if (c == '{' || c == '[') mode = JSON;
                else { mode = PASS; writeAll(STDOUT_FILENO, buf.data(), buf.size()); buf.clear(); continue; }
            }
            if (mode == JSON && buf.size() > kCap) { // too big to hold -- give up, stream it
                mode = PASS;
                writeAll(STDOUT_FILENO, buf.data(), buf.size());
                buf.clear();
            }
        }
    }

    if (haveSaved) tcsetattr(STDIN_FILENO, TCSANOW, &saved);

    // Flush whatever we were holding.
    if (mode == JSON) {
        std::string j;
        j.reserve(buf.size());
        for (char c : buf) if (c != '\r') j += c; // strip the PTY's ONLCR carriage returns
        Value v;
        if (fromJson(j, v)) {
            std::string out = renderText(v);
            writeAll(STDOUT_FILENO, out.data(), out.size());
            if (out.empty() || out.back() != '\n') { const char nl = '\n'; writeAll(STDOUT_FILENO, &nl, 1); }
        } else {
            writeAll(STDOUT_FILENO, buf.data(), buf.size()); // looked like JSON, wasn't -- show raw
        }
    } else if (mode == DECIDING && !buf.empty()) {
        writeAll(STDOUT_FILENO, buf.data(), buf.size()); // only whitespace / tiny output
    }

    close(master);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

} // namespace nucapture
