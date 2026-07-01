#pragma once
#include "history.h"
#include <functional>
#include <optional>
#include <string>
#include <termios.h>

// RAII raw-mode guard: disables ICANON/ECHO/ISIG for its lifetime, restoring
// whatever termios was in effect on destruction. Exposed (not local to
// edit.cpp) so callers outside readLine() -- e.g. main.cpp's chrome
// repaints -- can also suppress kernel echo for the moment they're writing
// their own cursor-positioning escape sequences, closing the race where a
// keystroke typed during that window gets echoed by the tty driver right in
// the middle of our own output stream.
struct RawMode {
    termios orig;
    RawMode();
    ~RawMode();
};

// Reads one line interactively using raw termios input, with history recall
// (Up/Down) and basic cursor movement (Left/Right/Backspace). Returns
// nullopt on EOF (Ctrl-D on an empty line).
//
// onIdleTick, if set, is invoked roughly once per second while waiting for
// the next keystroke (used to keep the pinned hardware-stats bar live even
// when the user isn't actively typing). It fires on a best-effort ~1Hz
// cadence tracked against an absolute deadline, so a burst of typing doesn't
// starve or drift it.
std::optional<std::string> readLine(const std::string& prompt, History& history,
                                     const std::function<void()>& onIdleTick = {});
