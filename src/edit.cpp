#include "edit.h"
#include "complete.h"
#include "highlight.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

RawMode::RawMode() {
    tcgetattr(STDIN_FILENO, &orig);
    termios raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}
RawMode::~RawMode() { tcsetattr(STDIN_FILENO, TCSANOW, &orig); }

static std::atomic<bool> g_tick{false};

void installIdleTicker() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = [](int) { g_tick.store(true, std::memory_order_relaxed); };
    sigemptyset(&sa.sa_mask);
    // Deliberately NOT SA_RESTART: this must interrupt a blocking read() so
    // the loop below gets a chance to check the flag even while sitting idle
    // waiting for the next keystroke.
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, nullptr);

    struct itimerval timer;
    timer.it_value.tv_sec = 1;
    timer.it_value.tv_usec = 0;
    timer.it_interval.tv_sec = 1;
    timer.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &timer, nullptr);
}

// Reads exactly one byte, transparently retrying on EINTR (SIGALRM/SIGWINCH
// landing mid-syscall). Used for every raw read in this file so a
// badly-timed signal can never truncate a multi-byte escape sequence --
// the outer loop in readLine() is what checks g_tick, once per full
// keystroke/action, not here.
static ssize_t readByte(char& out) {
    for (;;) {
        ssize_t n = read(STDIN_FILENO, &out, 1);
        if (n < 0 && errno == EINTR) continue;
        return n;
    }
}

// True if a byte is already available (or arrives within `timeoutMs`) on
// stdin. A real escape SEQUENCE (arrow keys etc.) arrives as a fast burst --
// the terminal constructs and sends the whole multi-byte sequence for one
// physical keypress essentially at once -- while a standalone Escape key
// press sends JUST the 0x1B byte, with nothing following. Used to
// distinguish the two instead of blocking forever waiting for continuation
// bytes that, for a standalone Escape, will never arrive on their own (real
// bug found live: pressing bare Escape hung ark's line editor indefinitely).
static bool byteAvailableSoon(int timeoutMs) {
    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int rv = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if (rv > 0) return true;
        if (rv == 0) return false;
        if (errno == EINTR) continue; // a tick/resize signal landing mid-wait
                                       // isn't a real answer either way -- retry
        return false;
    }
}

std::optional<std::string> readLine(const std::string& prompt, History& history,
                                     const std::function<void()>& onIdleTick) {
    RawMode raw;
    std::string buf;
    size_t cursor = 0;
    int histIndex = (int)history.lines().size(); // one-past-the-end = "not browsing history"

    std::cout << prompt << std::flush;

    auto redraw = [&]() {
        std::cout << "\r\x1b[K" << prompt << highlightLine(buf);
        size_t back = buf.size() - cursor;
        if (back > 0) std::cout << "\x1b[" << back << "D";
        std::cout << std::flush;
    };

    for (;;) {
        // Checked once per full keystroke/action (not just on a select()
        // timeout) so a continuous typing burst or a terminal paste -- where
        // read() keeps returning data immediately and never actually blocks
        // -- can't starve the tick indefinitely; it fires within one
        // keystroke of the SIGALRM that set it.
        if (onIdleTick && g_tick.exchange(false, std::memory_order_relaxed)) {
            onIdleTick();
        }

        char c;
        // NOT readByte() here, deliberately: readByte()'s internal EINTR
        // retry swallows the SIGALRM interruption before this loop ever
        // sees it, so while genuinely idle (blocked in read(), no bytes
        // arriving) the tick would never fire -- only the escape-sequence
        // continuation bytes below use readByte(), where transparently
        // retrying is correct because we're already committed to that
        // multi-byte parse. A raw read() here lets EINTR propagate back to
        // the top of the loop, where the tick actually gets checked, before
        // retrying the read.
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return std::nullopt; // EOF/error

        if (c == '\r' || c == '\n') { std::cout << "\n"; return buf; }
        if (c == 3) { std::cout << "\n"; return std::string(); } // Ctrl-C: cancel line
        if (c == 4 && buf.empty()) { std::cout << "\n"; return std::nullopt; } // Ctrl-D on empty line: EOF
        if (c == 127 || c == 8) { // Backspace
            if (cursor > 0) { buf.erase(cursor - 1, 1); cursor--; redraw(); }
            continue;
        }
        if (c == 9) { // Tab
            auto [wordStart, word] = wordUnderCursor(buf, cursor);
            bool cmdPos = isCommandPosition(buf, wordStart);
            auto candidates = cmdPos ? completeCommand(word) : completePath(word);
            if (candidates.empty()) { continue; }

            std::string prefix = longestCommonPrefix(candidates);
            if (prefix.size() > word.size()) {
                buf.replace(wordStart, word.size(), prefix);
                cursor = wordStart + prefix.size();
                if (candidates.size() == 1) {
                    char sep = (!cmdPos && isDirectory(prefix)) ? '/' : ' ';
                    buf.insert(cursor, 1, sep);
                    cursor++;
                }
                redraw();
            } else if (candidates.size() > 1) {
                std::cout << "\n";
                for (size_t i = 0; i < candidates.size(); i++) {
                    std::cout << candidates[i];
                    if (i + 1 < candidates.size()) std::cout << "  ";
                }
                std::cout << "\n";
                redraw();
            }
            continue;
        }
        if (c == '\x1b') { // escape sequence (arrow keys: ESC [ A/B/C/D) OR a
                            // standalone Escape key press
            if (!byteAvailableSoon(50)) {
                // Nothing followed within 50ms -- a real arrow-key sequence
                // would have. Treat as a standalone Escape: give it a
                // defined behavior (cancel the line, matching Ctrl-C)
                // instead of the previous silent hang waiting for
                // continuation bytes that were never coming.
                std::cout << "\n";
                return std::string();
            }
            char seq[2];
            if (readByte(seq[0]) <= 0) continue;
            if (readByte(seq[1]) <= 0) continue;
            if (seq[0] != '[') continue;
            if (seq[1] == 'C' && cursor < buf.size()) { cursor++; redraw(); }
            else if (seq[1] == 'D' && cursor > 0) { cursor--; redraw(); }
            else if (seq[1] == 'A') { // Up: older history
                if (histIndex > 0) { histIndex--; buf = history.lines()[histIndex]; cursor = buf.size(); redraw(); }
            } else if (seq[1] == 'B') { // Down: newer history
                if (histIndex < (int)history.lines().size()) {
                    histIndex++;
                    buf = histIndex == (int)history.lines().size() ? "" : history.lines()[histIndex];
                    cursor = buf.size();
                    redraw();
                }
            }
            continue;
        }
        buf.insert(cursor, 1, c);
        cursor++;
        redraw();
    }
}
