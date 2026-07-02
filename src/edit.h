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

// Arms a 1-second repeating SIGALRM used to drive onIdleTick (see readLine()
// below) on a true wall-clock cadence, independent of typing activity. Call
// once per session, before the first readLine() call.
void installIdleTicker();

// Reads one line interactively using raw termios input, with history recall
// (Up/Down) and basic cursor movement (Left/Right/Backspace). Returns
// nullopt on EOF (Ctrl-D on an empty line).
//
// onIdleTick, if set, is invoked roughly once per second (driven by
// installIdleTicker()'s SIGALRM, checked once per loop iteration here) to
// keep the pinned hardware-stats bar live. This does NOT wait for an idle
// gap in typing -- a naive "call it when select() times out waiting for the
// next byte" approach silently stops ticking during a fast typing burst or
// a terminal paste, since such a wait never times out while bytes keep
// arriving. Checking a signal-set flag once per character instead means the
// tick still lands within about one keystroke of its 1-second deadline no
// matter how fast input is arriving.
// isValidCommand, if set, powers real-time command validation in the syntax
// highlighter: a command-position word for which it returns false is painted
// red (a likely typo). It should resolve builtins / aliases / functions /
// $PATH / slash-paths -- readLine treats it as opaque. Empty = no validation.
std::optional<std::string> readLine(const std::string& prompt, History& history,
                                     const std::function<void()>& onIdleTick = {},
                                     const std::function<bool(const std::string&)>& isValidCommand = {});
