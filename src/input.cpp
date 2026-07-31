#include "input.h"
#include <cerrno>
#include <deque>
#include <sys/select.h>
#include <unistd.h>

namespace {
// A single process-wide FIFO of pending input bytes. Not thread-safe by design:
// all interactive input is read from the main thread (the REPL / line editor and
// the chrome reassert both run there), so no locking is needed.
std::deque<unsigned char> g_fifo;

int selectReadable(int timeoutMs) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
}
} // namespace

namespace arkinput {

void enqueue(const char* data, size_t n) {
    for (size_t i = 0; i < n; i++) g_fifo.push_back((unsigned char)data[i]);
}

long readByte(char& out, bool retryEINTR) {
    if (!g_fifo.empty()) {
        out = (char)g_fifo.front();
        g_fifo.pop_front();
        return 1;
    }
    for (;;) {
        ssize_t n = read(STDIN_FILENO, &out, 1);
        if (n < 0 && errno == EINTR && retryEINTR) continue;
        return (long)n;
    }
}

bool available(int timeoutMs) {
    if (!g_fifo.empty()) return true;
    for (;;) {
        int rv = selectReadable(timeoutMs);
        if (rv > 0) return true;
        if (rv == 0) return false;
        if (errno == EINTR) continue; // a tick/resize signal isn't an answer -- retry
        return false;
    }
}

int readRawTimed(char& out, int timeoutMs) {
    for (;;) {
        int rv = selectReadable(timeoutMs);
        if (rv == 0) return -2; // timeout
        if (rv < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        ssize_t n = read(STDIN_FILENO, &out, 1);
        return n == 1 ? 1 : (n == 0 ? 0 : -1);
    }
}

} // namespace arkinput
