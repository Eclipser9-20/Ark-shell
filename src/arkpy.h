#pragma once
#include "shell_state.h"
#include <string>
#include <vector>

// `ark-py [file.py]` -- an in-shell, IDE-style Python editor: a full-screen
// terminal editor with live Python syntax highlighting, identifier/keyword/
// builtin autocomplete (Tab), auto-indentation, and one-key run (Ctrl-R) that
// executes the buffer through the system `python3`. Ctrl-S saves, Ctrl-Q quits.
// Returns a shell exit status (0 on clean quit). Requires an interactive TTY.
int arkPyMain(const std::vector<std::string>& argv, ShellState& state);
