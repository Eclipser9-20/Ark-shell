#include "edit.h"
#include "complete.h"
#include "highlight.h"
#include <cerrno>
#include <chrono>
#include <iostream>
#include <sys/select.h>
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

// Blocks until STDIN has a byte ready or `deadline` passes. Returns true if a
// byte is ready to read, false on timeout. EINTR (e.g. a SIGWINCH landing
// mid-select) just retries against the same deadline rather than bubbling up
// as an error -- resizing the terminal shouldn't look like a dead input.
static bool waitForInput(std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining < std::chrono::microseconds(0)) remaining = std::chrono::microseconds(0);
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();
        struct timeval tv;
        tv.tv_sec = (long)(us / 1000000);
        tv.tv_usec = (long)(us % 1000000);

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int rv = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if (rv > 0) return true;
        if (rv == 0) return false;
        if (errno == EINTR) continue;
        return false; // treat any other select() error as "no input" -- the
                       // following read() will surface the real problem
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

    auto nextTick = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    for (;;) {
        if (onIdleTick) {
            if (!waitForInput(nextTick)) {
                onIdleTick();
                nextTick = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                continue;
            }
            nextTick = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
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
        if (c == '\x1b') { // escape sequence (arrow keys: ESC [ A/B/C/D)
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) <= 0) continue;
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
