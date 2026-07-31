#pragma once
#include <cstddef>

// ── Shared interactive stdin layer ──────────────────────────────────────────
// The line editor (edit.cpp) and the chrome's cursor-position query (chrome.cpp,
// via DSR "ESC[6n") both read from the same terminal fd. When they raced -- most
// visibly during a PASTE, where many bytes arrive at once -- the DSR reader ate
// the front of the paste while hunting for the reply's 'R', and a reply that
// arrived after the timeout leaked onto the command line as a literal
// "ESC[<row>;<col>R". Fix: every interactive read goes through this one tiny
// FIFO. The DSR reader consumes ONLY a well-formed cursor report and enqueue()s
// anything else (your keystrokes / paste) for the editor to read next, in order.
namespace arkinput {
    // Read one byte: from the FIFO if non-empty, else from stdin. Returns 1 on a
    // byte, 0 on EOF, -1 on read error, or -1 on EINTR when retryEINTR is false
    // (so a caller that wants a signal to interrupt its blocking read -- e.g. the
    // idle-tick loop -- still sees it). FIFO reads never block and never fail.
    long readByte(char& out, bool retryEINTR);

    // Append bytes to the BACK of the FIFO; they're returned by readByte() in the
    // order enqueued, ahead of anything still on the fd.
    void enqueue(const char* data, size_t n);

    // True if a byte can be had immediately: FIFO non-empty, or stdin readable
    // within timeoutMs (0 = just poll).
    bool available(int timeoutMs);

    // Read one byte straight off the fd (bypassing the FIFO), waiting up to
    // timeoutMs. For the DSR reader, which must read the terminal's fresh reply
    // rather than buffered user input. Returns 1 on a byte, 0 on EOF, -1 on
    // error, -2 on timeout.
    int readRawTimed(char& out, int timeoutMs);
}
