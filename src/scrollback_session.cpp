#include "scrollback_session.h"
#include "scrollback.h"
#include "vtmodel.h"
#include "jobs.h"
#include "chrome.h"   // arkScrollDebug
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
#ifdef __APPLE__
#include <util.h>      // openpty() on macOS/BSD
#else
#include <pty.h>       // openpty() on Linux (glibc)
#endif

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

long record(const std::string& text) {
    if (!enabled() || !g_ring) return -1;
    g_ring->push(scrollback::LogicalLine{text});
    return (long)g_ring->pushed() - 1;
}

namespace {
// Persistent line model for ark's OWN std::cout stream (builtin output, tables,
// banners). Separate from the per-command runForeground VtModel.
std::unique_ptr<scrollback::VtModel> g_outVt;
bool g_capturing = false;   // true only while a command executes (see setCapturing)
}

void setCapturing(bool on) {
    g_capturing = on;
    // Drop any half-formed line when capture turns off, so a command that didn't
    // end on a newline can't bleed into the next thing recorded.
    if (!on && g_outVt) g_outVt->takeCompleted();
}

void recordOutput(const char* data, size_t n) {
    if (!enabled() || !g_ring || !g_capturing || n == 0) return;
    if (!g_outVt) g_outVt = std::make_unique<scrollback::VtModel>(g_cols);
    g_outVt->feed(data, n);
    for (auto& l : g_outVt->takeCompleted()) g_ring->push(l);
}

// Return to the live bottom while PRESERVING the recent output: paint the ring's
// tail into the region (reserving the last row for the prompt) and leave the
// cursor on that last row for the caller's redraw. Without this, scrolling back
// down cleared the region and lost everything that was on screen.
void paintLiveTail() {
    if (!g_ring) return;
    int h = g_height > 1 ? g_height - 1 : 1;   // reserve the bottom row for the prompt
    g_ring->setViewport(g_cols, h);
    auto rows = g_ring->visibleRows();
    std::string out = "\x1b[?2026h";
    for (int i = 0; i < h; i++)
        out += "\x1b[" + std::to_string(g_top + i) + ";1H\x1b[2K" + rows[(size_t)i];
    out += "\x1b[" + std::to_string(g_bottom) + ";1H\x1b[2K";   // clear + park cursor on prompt row
    out += "\x1b[?2026l";
    (void)!write(STDOUT_FILENO, out.data(), out.size());
    g_ring->setViewport(g_cols, g_height);     // restore full viewport
}

void recolorLine(long seq, const std::string& text) {
    if (!enabled() || !g_ring || seq < 0) return;
    if (!g_ring->replaceSeq((size_t)seq, scrollback::LogicalLine{text})) return;
    if (g_ring->atLive()) paintLiveTail();   // reflect it in the live view now
    else paintRegion();
}

void scrollBy(int delta) {
    if (!enabled() || !g_ring) return;
    // Inert only when EVERYTHING fits in the live tail (which reserves the bottom
    // row for the prompt, so it shows g_height-1 ring rows). As soon as content
    // exceeds that -- even by one line, e.g. a couple of short commands -- the
    // wheel scrolls. (canScroll() alone compares against the full height and so
    // stayed inert for the one row hidden behind the prompt reservation.)
    int liveVisible = g_height > 1 ? g_height - 1 : 1;
    if (g_ring->atLive() && g_ring->physicalRows() <= liveVisible) return;
    // delta > 0 means "scroll toward older" (wheel up); Scrollback::scrollLines
    // uses the opposite sign convention, so negate.
    g_ring->scrollLines(-delta);
    g_scrolled = !g_ring->atLive();
    if (g_ring->atLive()) paintLiveTail();  // back at live: preserve the recent
                                            // output, caller redraws prompt at bottom
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
    paintLiveTail();   // preserve the recent output; caller redraws the prompt
}

int pageRows() {
    return g_height > 0 ? g_height : 1;
}

void onResize() {
    if (!enabled() || !g_ring) return;
    if (!measure()) return;
    g_ring->setViewport(g_cols, g_height);
    // Re-assert mouse reporting (a resize repaint may have reset terminal state)
    // and restore whatever the user was looking at.
    (void)!write(STDOUT_FILENO, "\x1b[?1000h\x1b[?1006h", 16);
    if (g_ring->atLive()) paintLiveTail();
    else paintRegion();
}

namespace {

struct SuspendedJob { int id; pid_t pid; int master; };
std::vector<SuspendedJob> g_suspended;   // suspended jobs, keyed by JobTable id

void eraseSuspendedJob(int id) {
    for (size_t i = 0; i < g_suspended.size(); i++)
        if (g_suspended[i].id == id) { g_suspended.erase(g_suspended.begin() + i); return; }
}

// Rewrite a child's raw output so its DECSTBM scroll-region changes can never
// widen the REAL terminal's region past the pinned-bar band. The child runs on
// its own PTY sized to the band height, but its bytes are streamed verbatim to
// the real terminal -- so a child that emits `ESC [ r` (full-region reset) or
// `ESC [ t;b r` re-margins the real screen, and its very next line feeds scroll
// the WHOLE screen, dragging the pinned top bar (row 1) out of place and the
// bottom bar (row N) off-screen. Reacting AFTER writing the chunk is too late
// when the reset and the scrolling newlines arrive in the SAME read() (a program
// that buffers `ESC[r` together with its output) -- the scroll has already
// happened. So instead, every DECSTBM in the stream is rewritten IN PLACE to our
// own band (`top..bottom`) before the bytes ever reach the terminal, preserving
// byte order so a same-chunk reset-then-scroll can never escape the band.
//
// A minimal escape parser is needed to avoid mistaking a literal 'r' inside an
// OSC/DCS string (or CSI parameters) for a DECSTBM final byte. `carry` holds an
// escape sequence that was split across a read() boundary so it can be completed
// on the next chunk; the caller must flush any trailing carry when the child
// exits (a dangling partial escape is written as-is).
std::string clampScrollRegion(const char* data, size_t n, std::string& carry,
                              int top, int bottom) {
    std::string in;
    in.reserve(carry.size() + n);
    in.append(carry);
    in.append(data, n);
    carry.clear();

    const std::string region = "\0337\x1b[" + std::to_string(top) + ";" +
                               std::to_string(bottom) + "r\0338";
    std::string out;
    out.reserve(in.size() + 16);

    size_t i = 0, sz = in.size();
    while (i < sz) {
        unsigned char c = (unsigned char)in[i];
        if (c != 0x1b) { out += in[i]; i++; continue; }
        // We are at an ESC. Determine the escape kind; if the sequence is not yet
        // complete within this buffer, stash it in `carry` and stop.
        if (i + 1 >= sz) { carry.assign(in, i, std::string::npos); break; }
        unsigned char c1 = (unsigned char)in[i + 1];
        if (c1 == '[') {
            // CSI: ESC [ (params/intermediates 0x20-0x3f)* final(0x40-0x7e)
            size_t j = i + 2;
            while (j < sz && (unsigned char)in[j] >= 0x20 && (unsigned char)in[j] <= 0x3f) j++;
            if (j >= sz) { carry.assign(in, i, std::string::npos); break; } // final not seen yet
            unsigned char fin = (unsigned char)in[j];
            if (fin == 'r') { out += region; arkScrollDebug("clamp: child DECSTBM rewritten to band"); }  // clamp to our band
            else out.append(in, i, j - i + 1);             // any other CSI: verbatim
            i = j + 1;
            continue;
        }
        if (c1 == ']' || c1 == 'P' || c1 == 'X' || c1 == '^' || c1 == '_') {
            // String sequence (OSC/DCS/SOS/PM/APC): copy through the BEL or ST
            // (ESC \) terminator without interpreting its body (which may contain
            // a literal 'r'). If the terminator hasn't arrived yet, carry the
            // whole thing to the next chunk.
            size_t j = i + 2;
            size_t end = std::string::npos;
            for (; j < sz; j++) {
                unsigned char b = (unsigned char)in[j];
                if (b == 0x07) { end = j + 1; break; }                        // BEL
                if (b == 0x1b) {
                    if (j + 1 < sz && in[j + 1] == '\\') { end = j + 2; break; } // ST
                    break; // bare ESC at/near buffer end: treat as split, carry below
                }
            }
            if (end == std::string::npos) { carry.assign(in, i, std::string::npos); break; }
            out.append(in, i, end - i);
            i = end;
            continue;
        }
        // Two-byte escape (ESC 7, ESC 8, ESC M, ESC c, ...): copy both bytes.
        out += in[i];
        out += in[i + 1];
        i += 2;
    }
    return out;
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

    arkScrollDebug("relay: START cmd='%s'  band=%d;%d cols=%d", cmdline.c_str(), g_top, g_bottom, g_cols);
    scrollback::VtModel vt(g_cols);
    bool alt = false, stopped = false;
    int status = 0;
    char buf[8192];
    std::string escCarry;   // partial escape sequence split across a read() (see clampScrollRegion)

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
            if (ev == scrollback::VtEvent::EnterAltScreen && !alt) {
                alt = true;
                // The child is taking the alt screen (a pager/editor): hand it the
                // full screen and drop any half-parsed DECSTBM rewrite state.
                escCarry.clear();
                arkScrollDebug("relay: ENTER alt-screen -> region dropped to full");
                (void)!write(STDOUT_FILENO, "\x1b[r", 3);
            }
            if (alt) {
                // Alt-screen apps own the whole display; pass their bytes through
                // untouched (they manage their own scroll region).
                (void)!write(STDOUT_FILENO, buf, (size_t)n);
            } else {
                // OVERLAY GUARD (proactive): rewrite every DECSTBM in the child's
                // output to OUR band (g_top..g_bottom) BEFORE it reaches the
                // terminal, so a full-region reset can never scroll the pinned
                // bars -- even when the reset and the scrolling newlines land in
                // the same read() chunk. clampScrollRegion holds any escape split
                // across a chunk boundary in escCarry for the next iteration.
                std::string outChunk = clampScrollRegion(buf, (size_t)n, escCarry, g_top, g_bottom);
                if (!outChunk.empty()) (void)!write(STDOUT_FILENO, outChunk.data(), outChunk.size());
                for (auto& l : vt.takeCompleted()) g_ring->push(l);
            }
            if (ev == scrollback::VtEvent::LeaveAltScreen && alt) {
                alt = false;
                // The pager/editor is done with the alt screen. RE-ASSERT ark's
                // band -- otherwise the region stays full-screen (dropped on
                // entry) and the child's remaining output, or the next command,
                // scrolls the whole screen and drags the pinned bars into the
                // flow (the `git log`-via-less leak). Bracketed DECSC/DECRC
                // because \x1b[...r homes the cursor on Ghostty.
                std::string re = "\0337\x1b[" + std::to_string(g_top) + ";" +
                                 std::to_string(g_bottom) + "r\0338";
                (void)!write(STDOUT_FILENO, re.data(), re.size());
                arkScrollDebug("relay: LEAVE alt-screen -> region re-asserted to %d;%d", g_top, g_bottom);
            }
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

    // Flush any escape sequence left dangling at the child's final read() as-is
    // (an unterminated escape is harmless; withholding it would drop bytes).
    if (!alt && !escCarry.empty()) (void)!write(STDOUT_FILENO, escCarry.data(), escCarry.size());
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
