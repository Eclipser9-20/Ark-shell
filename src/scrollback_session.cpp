#include "scrollback_session.h"
#include "scrollback.h"
#include "vtmodel.h"
#include "jobs.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <memory>
#include <vector>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <util.h>

namespace scrollback {

namespace {

std::unique_ptr<scrollback::Scrollback> g_ring;
bool g_scrolled = false;   // true while viewing history (offset > 0)
int g_top = 2, g_bottom = 23, g_height = 22, g_cols = 80;

bool measure() {
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_row == 0) return false;
    int rows = ws.ws_row;
    g_cols = ws.ws_col;
    // Region sits between the pinned bars: top bar row 1 (unless ARK_CHROME_TOP
    // says otherwise), bottom bar row N.
    const char* topEnv = getenv("ARK_CHROME_TOP");
    bool topPinned = !(topEnv && std::strcmp(topEnv, "pinned") != 0);
    g_top = topPinned ? 2 : 1;
    g_bottom = rows - 1;
    g_height = g_bottom - g_top + 1;
    if (g_height < 1) g_height = 1;
    return true;
}

int ringCap() {
    const char* v = getenv("ARK_SCROLLBACK_LINES");
    if (v) { int n = atoi(v); if (n > 100) return n; }
    return 10000;
}

// Paint the viewport (history window) into the region rows, plus a hint on the
// last region row while scrolled up.
void paintRegion() {
    if (!g_ring) return;
    std::string out = "\x1b[?2026h";      // synchronized update
    auto rows = g_ring->visibleRows();
    for (int i = 0; i < g_height; i++) {
        out += "\x1b[" + std::to_string(g_top + i) + ";1H\x1b[2K";
        out += rows[(size_t)i];
    }
    if (!g_ring->atLive()) {
        out += "\x1b[" + std::to_string(g_bottom) + ";1H";
        out += "\x1b[7m \xe2\x96\xbc " + std::to_string(g_ring->pendingBelow()) + " below \x1b[0m";
    }
    out += "\x1b[?2026l";
    (void)!write(STDOUT_FILENO, out.data(), out.size());
}

} // namespace

bool enabled() {
    const char* v = getenv("ARK_SCROLLBACK");
    if (!v || std::strcmp(v, "1") != 0) return false;
    if (getenv("ARK_DEFAULT_TERMINAL")) return false;
    if (getenv("CI")) return false;
    return isatty(STDOUT_FILENO) && isatty(STDIN_FILENO);
}

void init() {
    if (!enabled()) return;
    if (!measure()) return;
    if (!g_ring) g_ring = std::make_unique<scrollback::Scrollback>((size_t)ringCap());
    g_ring->setViewport(g_cols, g_height);
    // (Re)enable SGR mouse reporting so the wheel drives scroll-back -- re-asserted
    // every init because chrome's per-command cleanup would otherwise clear it.
    // Shift+drag still does native copy (terminal honors the Shift override).
    (void)!write(STDOUT_FILENO, "\x1b[?1000h\x1b[?1006h", 16);
}

void record(const std::string& text) {
    if (!enabled() || !g_ring) return;
    g_ring->push(scrollback::LogicalLine{text});
}

namespace { void clearRegion() {
    std::string out;
    for (int i = 0; i < g_height; i++)
        out += "\x1b[" + std::to_string(g_top + i) + ";1H\x1b[2K";
    (void)!write(STDOUT_FILENO, out.data(), out.size());
} }

void scrollBy(int delta) {
    if (!enabled() || !g_ring) return;
    // Nothing to scroll for (only the prompt on screen, no scrollback yet): do
    // nothing -- not even a repaint -- so the wheel is inert until history exists.
    if (g_ring->atLive() && !g_ring->canScroll()) return;
    // delta > 0 means "scroll toward older" (wheel up); Scrollback::scrollLines
    // uses the opposite sign convention, so negate.
    g_ring->scrollLines(-delta);
    g_scrolled = !g_ring->atLive();
    if (g_ring->atLive()) clearRegion();   // back at live: clear stale history so
                                            // the caller can redraw the live prompt
    else paintRegion();
}

bool atLive() {
    if (!enabled() || !g_ring) return true;
    return g_ring->atLive();
}

void snapToLive() {
    if (!enabled() || !g_ring) return;
    if (g_ring->atLive() && !g_scrolled) return;
    g_ring->snapToLive();
    g_scrolled = false;
    // The live prompt is redrawn by the caller (readLine's redraw); we just
    // clear the region so stale history rows don't linger under the prompt.
    std::string out;
    for (int i = 0; i < g_height; i++)
        out += "\x1b[" + std::to_string(g_top + i) + ";1H\x1b[2K";
    (void)!write(STDOUT_FILENO, out.data(), out.size());
}

int pageRows() {
    return g_height > 0 ? g_height : 1;
}

namespace {

struct SuspendedJob { int id; pid_t pid; int master; };
std::vector<SuspendedJob> g_suspended;   // suspended jobs, keyed by JobTable id

void eraseSuspendedJob(int id) {
    for (size_t i = 0; i < g_suspended.size(); i++)
        if (g_suspended[i].id == id) { g_suspended.erase(g_suspended.begin() + i); return; }
}

// Drive the PTY relay for an already-running child. jobId < 0 = a fresh command;
// >= 0 = resuming an existing job. Displays output live + records it, forwards
// stdin. Returns the exit status, or STOPPED (128+SIGTSTP) if the child was
// Ctrl-Z'd -- in which case a Stopped JobTable job is registered/kept and the
// PTY master is preserved so resume() can pick it back up.
constexpr int STOPPED = 128 + SIGTSTP;

int relay(pid_t pid, int master, JobTable& jobs, int jobId, const std::string& cmdline) {
    struct termios orig{}, raw{};
    tcgetattr(STDIN_FILENO, &orig);
    raw = orig; cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    scrollback::VtModel vt(g_cols);
    bool alt = false, stopped = false;
    int status = 0;
    char buf[8192];

    for (;;) {
        int wst;
        pid_t wr = waitpid(pid, &wst, WNOHANG | WUNTRACED);
        if (wr == pid && WIFSTOPPED(wst)) { stopped = true; break; }
        if (wr == pid && (WIFEXITED(wst) || WIFSIGNALED(wst))) {
            status = WIFEXITED(wst) ? WEXITSTATUS(wst) : 128 + WTERMSIG(wst); break;
        }
        fd_set fds; FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds); FD_SET(master, &fds);
        int mx = master > STDIN_FILENO ? master : STDIN_FILENO;
        struct timeval tv{0, 100000};
        int rv = select(mx + 1, &fds, nullptr, nullptr, &tv);
        if (rv < 0) { if (errno == EINTR) continue; break; }
        if (rv == 0) continue;
        if (FD_ISSET(master, &fds)) {
            ssize_t n = read(master, buf, sizeof buf);
            if (n <= 0) { waitpid(pid, &wst, 0); status = WIFEXITED(wst) ? WEXITSTATUS(wst) : 128 + WTERMSIG(wst); break; }
            scrollback::VtEvent ev = vt.feed(buf, (size_t)n);
            if (ev == scrollback::VtEvent::EnterAltScreen && !alt) { alt = true; (void)!write(STDOUT_FILENO, "\x1b[r", 3); }
            (void)!write(STDOUT_FILENO, buf, (size_t)n);
            if (!alt) for (auto& l : vt.takeCompleted()) g_ring->push(l);
            if (ev == scrollback::VtEvent::LeaveAltScreen && alt) alt = false;
        }
        if (FD_ISSET(STDIN_FILENO, &fds)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) break;
            // The child is a session leader in an orphaned process group, so a
            // stop signal (Ctrl-Z) is discarded by the kernel -- true suspend
            // needs a per-command supervisor process (a follow-up). For now
            // Ctrl-Z cancels the command instead of silently doing nothing.
            bool ctrlZ = false;
            for (ssize_t i = 0; i < n; i++) if (buf[i] == 0x1a) ctrlZ = true;
            if (ctrlZ) {
                kill(-pid, SIGTERM);
                (void)!write(STDOUT_FILENO, "\r\n\x1b[2m[cancelled -- suspend not supported yet]\x1b[0m\r\n", 49);
                continue;
            }
            (void)!write(master, buf, (size_t)n);
        }
    }

    std::string tail = vt.partial();
    if (!alt && !tail.empty()) g_ring->push(scrollback::LogicalLine{tail});
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);

    if (stopped) {
        // Register (fresh) or keep (resumed) a Stopped job; keep the PTY open.
        if (jobId < 0) {
            int id = jobs.add(pid, {pid}, cmdline);
            if (Job* j = jobs.find(id)) j->state = Job::State::Stopped;
            g_suspended.push_back({id, pid, master});
        } else if (Job* j = jobs.find(jobId)) {
            j->state = Job::State::Stopped;
        }
        (void)!write(STDOUT_FILENO, "\r\n\x1b[2m[stopped]\x1b[0m\r\n", 22);
        return STOPPED;
    }
    // Exited: clean up any job record and the PTY.
    if (jobId >= 0) { jobs.remove(jobId); eraseSuspendedJob(jobId); }
    close(master);
    return status;
}

} // namespace

int runForeground(const std::vector<std::string>& argv, JobTable& jobs) {
    if (!enabled() || !g_ring) return -1;
    if (!measure()) return -1;

    int master, slave;
    struct winsize ws{}; ws.ws_row = (unsigned short)g_height; ws.ws_col = (unsigned short)g_cols;
    if (openpty(&master, &slave, nullptr, nullptr, &ws) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(master); close(slave); return -1; }
    if (pid == 0) {
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
        if (slave > 2) close(slave);
        close(master);
        signal(SIGINT, SIG_DFL); signal(SIGQUIT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL); signal(SIGTTOU, SIG_DFL);
        std::vector<char*> cargv;
        for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    close(slave);

    std::string cmdline;
    for (size_t i = 0; i < argv.size(); i++) { if (i) cmdline += ' '; cmdline += argv[i]; }
    return relay(pid, master, jobs, /*jobId=*/-1, cmdline);
}

bool isSuspendedJob(int jobId) {
    for (auto& j : g_suspended) if (j.id == jobId) return true;
    return false;
}

int resume(JobTable& jobs, int jobId) {
    SuspendedJob* mj = nullptr;
    for (auto& j : g_suspended) if (j.id == jobId) { mj = &j; break; }
    if (!mj) return -1;
    measure();
    if (Job* j = jobs.find(jobId)) j->state = Job::State::Running;
    kill(-mj->pid, SIGCONT);
    return relay(mj->pid, mj->master, jobs, jobId, "");
}

} // namespace scrollback
